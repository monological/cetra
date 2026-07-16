#version 330 core
in vec3 Normal;
in vec3 WorldPos;
in vec3 ViewPos;
in vec3 FragPos;
in float ClipDepth;
in float FragDepth;
in vec2 TexCoords;
in vec2 TexCoords2;   // UV1 for lightmaps/AO
in vec4 VertexColor;  // Vertex color (RGBA)
in mat3 TBN;
layout(location = 0) out vec4 FragColor;
// G-buffer for screen-space passes: view-space normal (xyz) + roughness (a).
// Only lands when the engine enables color attachment 1; otherwise discarded.
layout(location = 1) out vec4 NormalOut;

#define MAX_LIGHTS 70

struct Light {
    int type;
    vec3 position;
    vec3 direction;
    vec3 color;
    vec3 specular;
    vec3 ambient;
    float intensity;
    float constant;
    float linear;
    float quadratic;
    float cutOff;
    float outerCutOff;
    vec2 size;
};

uniform Light lights[MAX_LIGHTS];
uniform int numLights;

uniform mat4 view;
uniform mat4 model;
uniform mat4 projection;

uniform int renderMode;
uniform float nearClip;
uniform float farClip;

uniform vec3 albedo;
uniform vec3 emissiveFactor;  // Emissive color factor (multiplied with emissive texture)
uniform float metallic;
uniform float roughness;
uniform float ao;
uniform float materialOpacity;
uniform float alphaCutoff;  // Alpha cutoff threshold for hair/foliage (0 = disabled)
// Alpha-to-coverage: MSAA turns fractional alpha into sample coverage, so the
// binary cutoff is skipped for soft, order-independent masked edges
uniform int alphaToCoverage;
// Geometric specular AA strength (0 disables)
uniform float specularAAStrength;

// With alpha-to-coverage active only fully invisible fragments are discarded
// (fractional alpha becomes MSAA coverage); otherwise the binary cutoff
// applies (alphaCutoff of 0 never discards since alpha is non-negative)
const float A2C_MIN_ALPHA = 0.02;

bool alphaBelowCutoff(float a)
{
    return a < (alphaToCoverage > 0 ? A2C_MIN_ALPHA : alphaCutoff);
}
uniform float normalScale;  // Normal map intensity scale (1.0 = full strength)
uniform float aoStrength;   // Occlusion texture strength (1.0 = full effect)
uniform float ior;
uniform float filmThickness;
uniform vec2 uvOffset;      // Texture coordinate offset (KHR_texture_transform)
uniform vec2 uvScale;       // Texture coordinate scale (KHR_texture_transform)
uniform float uvRotation;   // Texture coordinate rotation in radians
uniform int vertexColorExists;  // Whether mesh has vertex colors
uniform int texCoords2Exists;   // Whether mesh has UV1
uniform vec3 camPos;
uniform float time;

uniform sampler2D albedoTex;
uniform sampler2D normalTex;
uniform sampler2D roughnessTex;
uniform sampler2D metalnessTex;
uniform sampler2D aoTex;
uniform sampler2D emissiveTex;
uniform sampler2D heightTex;
uniform sampler2D opacityTex;
uniform sampler2D sheenTex;
uniform sampler2D reflectanceTex;
uniform sampler2D microsurfaceTex;
uniform sampler2D anisotropyTex;
uniform sampler2D subsurfaceTex;

uniform int albedoTexExists;
uniform int normalTexExists;
uniform int roughnessTexExists;
uniform int metalnessTexExists;
uniform int aoTexExists;
uniform int emissiveTexExists;
uniform int heightTexExists;
uniform int opacityTexExists;
uniform int sheenTexExists;
uniform int reflectanceTexExists;
uniform int microsurfaceTexExists;
uniform int anisotropyTexExists;
uniform int subsurfaceTexExists;

// Shadow mapping uniforms
#define MAX_SHADOW_LIGHTS 3
uniform sampler2DArray shadowMaps;
uniform mat4 lightSpaceMatrix[MAX_SHADOW_LIGHTS];
uniform int shadowLightIndex[MAX_SHADOW_LIGHTS];
uniform int numShadowLights;
uniform float shadowBias;
uniform vec2 shadowTexelSize;
// PCSS (contact-hardening shadows). When disabled the 3x3 PCF fallback
// runs, bit-identical to the pre-PCSS path. The ortho shadow projection
// stores depth linearly in [near,far], so the blocker/receiver separation
// that sets the penumbra width is measured on linearized depths.
uniform int pcssEnabled;
uniform float pcssSoftness;      // Multiplier on the light's angular size
uniform float shadowFrustumWidth; // World units across the ortho shadow map
uniform vec2 shadowNearFar;       // Ortho near/far planes (world units)

// IBL (Image-Based Lighting) uniforms
uniform samplerCube irradianceMap;
uniform samplerCube prefilteredMap;
uniform sampler2D brdfLUT;
uniform int iblEnabled;
uniform float iblIntensity;
uniform float maxReflectionLOD;

const float PI = 3.14159265359;

// UV transform for KHR_texture_transform
vec2 transformUV(vec2 uv) {
    // Apply rotation around origin
    float s = sin(uvRotation);
    float c = cos(uvRotation);
    vec2 rotated = vec2(uv.x * c - uv.y * s, uv.x * s + uv.y * c);
    // Apply scale and offset
    return rotated * uvScale + uvOffset;
}

// Color space conversions
vec3 sRGBToLinear(vec3 srgb) {
    return pow(srgb, vec3(2.2));
}

vec3 linearToSRGB(vec3 linear) {
    return pow(linear, vec3(1.0 / 2.2));
}

// Fresnel-Schlick approximation
vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Fresnel-Schlick with roughness for IBL
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Thin-film interference for iridescent coatings (pilot visor effect)
// thickness: film thickness in nanometers (200-600nm typical)
// cosTheta: dot(N, V) - viewing angle
// filmIOR: refractive index of the thin film coating (~1.5 for most coatings)
vec3 thinFilmInterference(float thickness, float cosTheta, float filmIOR) {
    // Wavelengths in nanometers for RGB
    const vec3 wavelengths = vec3(650.0, 550.0, 450.0); // R, G, B

    // Refracted angle in the film (Snell's law, assuming air n=1.0).
    // cosTheta comes from a normalized dot product that can round above 1;
    // an unclamped square makes these sqrt arguments negative, and the NaN
    // poisons the whole light sum into black view-dependent flecks
    cosTheta = clamp(cosTheta, 0.0, 1.0);
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    float sinThetaFilm = sinTheta / filmIOR;
    float cosThetaFilm = sqrt(max(1.0 - sinThetaFilm * sinThetaFilm, 0.0));

    // Optical path difference (2 * n * d * cos(theta_film))
    float opd = 2.0 * filmIOR * thickness * cosThetaFilm;

    // Phase shift for each wavelength
    vec3 phase = 2.0 * PI * opd / wavelengths;

    // Interference: (1 + cos(phase)) / 2 gives 0-1 range
    // Add phase shift of PI for reflection from denser medium
    vec3 interference = 0.5 + 0.5 * cos(phase + PI);

    // Boost saturation for more vivid colors
    vec3 color = interference;
    float avg = (color.r + color.g + color.b) / 3.0;
    color = mix(vec3(avg), color, 1.5); // Increase saturation

    return clamp(color, 0.0, 1.0);
}

// GGX/Trowbridge-Reitz Normal Distribution Function
float distributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float num = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;

    return num / denom;
}

// Anisotropic GGX distribution (for brushed metal, hair, etc.)
float distributionGGXAnisotropic(vec3 N, vec3 H, vec3 T, vec3 B, float roughness, float anisotropy) {
    float at = max(roughness * (1.0 + anisotropy), 0.001);
    float ab = max(roughness * (1.0 - anisotropy), 0.001);

    float ToH = dot(T, H);
    float BoH = dot(B, H);
    float NoH = max(dot(N, H), 0.0);

    float a2 = at * ab;
    vec3 v = vec3(ab * ToH, at * BoH, a2 * NoH);
    float v2 = dot(v, v);
    float w2 = a2 / v2;

    return a2 * w2 * w2 / PI;
}

// Subsurface scattering approximation using wrap lighting
vec3 subsurfaceScattering(vec3 N, vec3 L, vec3 V, vec3 albedo, float thickness, vec3 lightColor) {
    // Wrap lighting for diffuse transmission
    float wrap = 0.5;
    float NdotL = dot(N, L);
    float wrapDiffuse = max(0.0, (NdotL + wrap) / (1.0 + wrap));

    // Back-lighting transmission
    float transmittance = exp(-thickness * 2.0);
    vec3 backLight = albedo * lightColor * transmittance * max(0.0, -NdotL);

    return backLight * 0.5;
}

// Smith's Schlick-GGX geometry function for a single direction
float geometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;

    float num = NdotV;
    float denom = NdotV * (1.0 - k) + k;

    return num / denom;
}

// Smith's geometry function combining view and light directions
float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = geometrySchlickGGX(NdotV, roughness);
    float ggx1 = geometrySchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
}

// Attenuation for point/spot lights
float calculateAttenuation(float distance, float constant, float linear, float quadratic) {
    return 1.0 / (constant + linear * distance + quadratic * (distance * distance));
}

// 16-tap Poisson disk (unit radius) for the PCSS blocker search and filter.
// Sampled UNROTATED: a per-pixel rotation decorrelates the pattern between
// neighbours, which turns the 16-tap quantization into per-pixel shadow noise
// — invisible on diffuse but riding the sharp specular lobe into a field of
// bright speckle on the metal. An unrotated disk gives a spatially coherent,
// smooth penumbra instead; the modest radius cap keeps 16 taps free of banding.
const vec2 POISSON16[16] = vec2[](
    vec2(-0.9420, -0.3991), vec2(0.9456, -0.7689), vec2(-0.0942, -0.9294),
    vec2(0.3450, 0.2939), vec2(-0.9159, 0.4577), vec2(-0.8154, -0.8791),
    vec2(-0.3828, 0.2768), vec2(0.9748, 0.7565), vec2(0.4432, -0.9751),
    vec2(0.5374, -0.4737), vec2(-0.2650, -0.4189), vec2(0.7920, 0.1909),
    vec2(-0.2419, 0.9971), vec2(-0.8141, 0.9144), vec2(0.1998, 0.7864),
    vec2(0.1438, -0.1410));

// Max PCSS filter/search radius in shadow-UV. Raising this past ~10 texels
// (2048 map) reintroduces shadow noise that rides the specular into speckle
// with a 16-tap unrotated disk — a hard cleanliness cap, not a look knob.
const float PCSS_MAX_RADIUS_UV = 0.005;
// Penumbra growth per unit of blocker-receiver separation.
const float PCSS_PENUMBRA_SCALE = 4.0;

// Ortho shadow depth is linear in [near,far]; recover world-space distance
// from the light so blocker/receiver separation is a real length
float linearizeOrthoDepth(float d01) {
    return shadowNearFar.x + d01 * (shadowNearFar.y - shadowNearFar.x);
}

// Fixed 3x3 PCF: the pre-PCSS path, kept bit-identical as the fallback
float shadowPCF3x3(int shadowIndex, vec2 uv, float currentDepth, float bias) {
    float shadow = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            vec2 offset = vec2(float(x), float(y)) * shadowTexelSize;
            float pcfDepth = texture(shadowMaps, vec3(uv + offset, float(shadowIndex))).r;
            shadow += currentDepth - bias > pcfDepth ? 1.0 : 0.0;
        }
    }
    return 1.0 - (shadow / 9.0);
}

// Contact-hardening soft shadow (PCSS). lightSize is the scalar emitter size
// (world units of the emitter disk); larger = softer, faster-growing penumbra
// with blocker distance. The algorithm is isotropic, so callers collapse a
// rectangular emitter to a single dimension.
float calculateShadow(int shadowIndex, vec3 worldPos, float NdotL, float lightSize) {
    vec4 fragPosLightSpace = lightSpaceMatrix[shadowIndex] * vec4(worldPos, 1.0);
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;

    if (projCoords.z > 1.0 || projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0) {
        return 1.0;
    }

    float bias = max(shadowBias * (1.0 - NdotL), shadowBias * 0.1);
    float currentDepth = projCoords.z;

    if (pcssEnabled == 0) {
        return shadowPCF3x3(shadowIndex, projCoords.xy, currentDepth, bias);
    }

    // The emitter as a fraction of the shadow map. Capped low so the 16 taps
    // stay dense enough to be free of banding, and so the penumbra saturates
    // cleanly rather than growing past what the tap budget can resolve; the
    // penumbra still grows with blocker distance for contact hardening.
    float lightSizeUV =
        clamp(pcssSoftness * lightSize / shadowFrustumWidth, 0.0, PCSS_MAX_RADIUS_UV);
    if (lightSizeUV < shadowTexelSize.x) {
        // Emitter smaller than a texel: no meaningful penumbra, stay crisp
        return shadowPCF3x3(shadowIndex, projCoords.xy, currentDepth, bias);
    }

    // 1. Blocker search: average depth of texels nearer the light than the
    //    receiver, over a disk the size of the emitter's shadow footprint
    float zReceiver = linearizeOrthoDepth(currentDepth);
    float blockerSum = 0.0;
    float blockerCount = 0.0;
    for (int i = 0; i < 16; i++) {
        vec2 off = POISSON16[i] * lightSizeUV;
        float d = texture(shadowMaps, vec3(projCoords.xy + off, float(shadowIndex))).r;
        if (d < currentDepth - bias) {
            blockerSum += linearizeOrthoDepth(d);
            blockerCount += 1.0;
        }
    }
    if (blockerCount < 0.5) {
        return 1.0; // no blockers: fully lit
    }
    float zBlocker = blockerSum / blockerCount;

    // 2. Penumbra: grows with blocker-receiver separation (feet touching the
    //    floor stay sharp; the head 2m up casts a soft edge). Normalized by
    //    frustum depth so the product with lightSizeUV is a clean ratio.
    float penumbra = (zReceiver - zBlocker) / (shadowNearFar.y - shadowNearFar.x);
    float filterRadiusUV =
        clamp(lightSizeUV * penumbra * PCSS_PENUMBRA_SCALE, shadowTexelSize.x, PCSS_MAX_RADIUS_UV);

    // 3. Variable-width PCF over the same disk
    float shadow = 0.0;
    for (int i = 0; i < 16; i++) {
        vec2 off = POISSON16[i] * filterRadiusUV;
        float d = texture(shadowMaps, vec3(projCoords.xy + off, float(shadowIndex))).r;
        shadow += currentDepth - bias > d ? 1.0 : 0.0;
    }
    return 1.0 - (shadow / 16.0);
}

void main() {
    // Early-out for simple render modes that don't need texture sampling
    if (renderMode == 5) {
        // Flat Color - no textures needed
        FragColor = vec4(1.0, 0.5, 0.2, 1.0);
        return;
    }
    if (renderMode == 1) {
        // Normals Visualization - no textures needed
        vec3 color = normalize(Normal) * 0.5 + 0.5;
        FragColor = vec4(color, 1.0);
        return;
    }
    if (renderMode == 2) {
        // World Position Visualization - no textures needed
        vec3 color = fract(WorldPos * 0.01);
        FragColor = vec4(color, 1.0);
        return;
    }
    if (renderMode == 3) {
        // Texture Coordinates Visualization - no textures needed
        FragColor = vec4(TexCoords, 0.0, 1.0);
        return;
    }
    if (renderMode == 4) {
        // Tangent Space Visualization - no textures needed
        vec3 tangent = normalize(TBN[0]) * 0.5 + 0.5;
        FragColor = vec4(tangent, 1.0);
        return;
    }
    if (renderMode == 6) {
        // Albedo Only - only sample albedo texture
        vec2 uvAlbedo = transformUV(TexCoords);
        vec3 albedoMapOnly = albedo;
        float texAlphaOnly = 1.0;
        if (albedoTexExists > 0) {
            // sRGB texture: the hardware already decoded the sample to linear
            vec4 albedoSample = texture(albedoTex, uvAlbedo);
            albedoMapOnly = albedo * albedoSample.rgb;
            texAlphaOnly = albedoSample.a;
        }
        // Apply vertex color
        if (vertexColorExists > 0) {
            albedoMapOnly *= sRGBToLinear(VertexColor.rgb);
            texAlphaOnly *= VertexColor.a;
        }
        // Alpha cutoff for hair/foliage
        if (alphaBelowCutoff(texAlphaOnly)) {
            discard;
        }
        vec3 color = linearToSRGB(albedoMapOnly);
        FragColor = vec4(color, materialOpacity * texAlphaOnly);
        return;
    }

    // Apply UV transform for KHR_texture_transform
    vec2 uv = transformUV(TexCoords);

    // Sample material properties. glTF semantics: the scalar factors
    // modulate the texture (effective value = factor * texture), so a
    // material can globally tint or gloss up its maps
    vec3 albedoMap = albedo;
    float texAlpha = 1.0;  // Alpha from albedo texture (for hair/foliage)
    if (albedoTexExists > 0) {
        // sRGB texture: the hardware already decoded the sample to linear
        vec4 albedoSample = texture(albedoTex, uv);
        albedoMap = albedo * albedoSample.rgb;
        texAlpha = albedoSample.a;
    }

    // Apply vertex color to tint albedo (glTF vertex colors)
    if (vertexColorExists > 0) {
        albedoMap *= sRGBToLinear(VertexColor.rgb);
        texAlpha *= VertexColor.a;
    }

    // Alpha cutoff for hair/foliage - discard early before expensive lighting
    if (alphaBelowCutoff(texAlpha)) {
        discard;
    }

    vec3 N;
    if (normalTexExists > 0) {
        // Re-orthonormalize the interpolated TBN (Gram-Schmidt) before
        // applying the normal map. Vertex interpolation — and especially
        // skinning, where cross-rig retargeting puts non-uniform scale/shear
        // in the bone matrices — leaves the raw TBN non-orthonormal, which
        // skews the normal-mapped normal (worst where the normal map is
        // steep) into bright specks under the key lights on deformed poses.
        // The geometric normal itself stays clean, so this never showed in
        // the Normals render view (which visualizes the geometric normal).
        vec3 Ng = normalize(Normal);
        vec3 T = normalize(TBN[0] - dot(TBN[0], Ng) * Ng);
        vec3 B = cross(Ng, T);
        if (dot(B, TBN[1]) < 0.0) {
            B = -B; // preserve the mesh's authored bitangent handedness
        }
        vec3 nTex = texture(normalTex, uv).rgb * 2.0 - 1.0;
        // Apply normal scale to XY components (glTF normalTexture.scale)
        nTex.xy *= normalScale;
        N = normalize(mat3(T, B, Ng) * nTex);
    } else {
        N = normalize(Normal);
    }
    // Double-sided surfaces (hair cards, foliage): the back face must be lit
    // as its own side, or it faces away from every light and renders black
    if (!gl_FrontFacing) {
        N = -N;
    }

    float roughnessMap = roughness;
    if (roughnessTexExists > 0) {
        // glTF: G channel contains roughness (works for grayscale too since R=G=B)
        roughnessMap = roughness * texture(roughnessTex, uv).g;
    }
    // Clamp roughness to avoid division issues
    roughnessMap = clamp(roughnessMap, 0.04, 1.0);

    float metallicMap = metallic;
    if (metalnessTexExists > 0) {
        // glTF: B channel contains metallic (works for grayscale too since R=G=B)
        metallicMap = metallic * texture(metalnessTex, uv).b;
    }

    float aoMap = ao;
    if (aoTexExists > 0) {
        // Use UV1 for AO if available (common glTF lightmap pattern), otherwise UV0
        vec2 aoUV = (texCoords2Exists > 0) ? TexCoords2 : uv;
        // Apply occlusion strength (glTF occlusionTexture.strength)
        float sampledAo = texture(aoTex, aoUV).r;
        aoMap = mix(1.0, sampledAo, aoStrength);
    }

    vec3 emissiveMap = vec3(0.0);
    if (emissiveTexExists > 0) {
        // sRGB texture: the hardware already decoded the sample to linear
        vec3 texEmissive = texture(emissiveTex, uv).rgb;
        // Scale by emissiveFactor if set, otherwise use texture directly (backward compat)
        float factorSum = emissiveFactor.r + emissiveFactor.g + emissiveFactor.b;
        emissiveMap = texEmissive * (factorSum > 0.001 ? emissiveFactor : vec3(1.0));
    } else {
        emissiveMap = emissiveFactor;
    }

    float opacity = materialOpacity;
    if (opacityTexExists > 0) {
        opacity = texture(opacityTex, uv).r * materialOpacity;
    } else if (texAlpha < 1.0) {
        // Use albedo texture alpha if no separate opacity texture
        opacity = texAlpha * materialOpacity;
    }

    // Microsurface detail - modulates roughness for fine surface detail
    if (microsurfaceTexExists > 0) {
        float detail = texture(microsurfaceTex, uv).r;
        roughnessMap = clamp(roughnessMap * (0.5 + detail), 0.04, 1.0);
    }

    // Anisotropy - for brushed metal, hair effects
    float anisotropyMap = 0.0;
    if (anisotropyTexExists > 0) {
        anisotropyMap = texture(anisotropyTex, uv).r;
    }

    // Subsurface scattering thickness map
    float sssThickness = 1.0;
    if (subsurfaceTexExists > 0) {
        sssThickness = texture(subsurfaceTex, uv).r;
    }

    // Geometric specular anti-aliasing (Kaplanyan 2016): where the normal
    // varies quickly within a pixel (fine normal-mapped detail under
    // minification), the true reflectance is a wider lobe than any single
    // normal's — widen roughness by the normal variance to match, killing
    // the sparkle/shimmer that GGX otherwise produces in motion
    if (specularAAStrength > 0.0) {
        vec3 dndu = dFdx(N);
        vec3 dndv = dFdy(N);
        float variance =
            specularAAStrength * specularAAStrength * 0.25 * (dot(dndu, dndu) + dot(dndv, dndv));
        float kernelRoughness2 = min(2.0 * variance, 0.18);
        float a2 = roughnessMap * roughnessMap;
        roughnessMap = clamp(sqrt(a2 + kernelRoughness2), roughnessMap, 1.0);
    }

    // Calculate view direction (must use WorldPos, not FragPos which is clip space)
    vec3 V = normalize(camPos - WorldPos);

    // Render modes that need texture data
    if (renderMode == 7) {
        // Simple Diffuse Lighting
        vec3 Lo = vec3(0.0);
        for (int i = 0; i < numLights; i++) {
            vec3 L;
            float attenuation;
            if (lights[i].type == 0) {
                L = normalize(-lights[i].direction);
                attenuation = 1.0;
            } else {
                L = normalize(lights[i].position - WorldPos);
                float distance = length(lights[i].position - WorldPos);
                attenuation = calculateAttenuation(distance, lights[i].constant,
                                                   lights[i].linear, lights[i].quadratic);
            }
            float NdotL = max(dot(N, L), 0.0);
            Lo += albedoMap * lights[i].color * lights[i].intensity * attenuation * NdotL;
        }
        vec3 color = Lo + vec3(0.03) * albedoMap;
        color = color / (color + vec3(1.0));
        color = linearToSRGB(color);
        FragColor = vec4(color, opacity);
        return;
    }
    if (renderMode == 8) {
        // Metallic and Roughness Visualization
        FragColor = vec4(metallicMap, roughnessMap, 0.0, 1.0);
        return;
    }

    // renderMode == 0: Full PBR

    // Calculate F0 (surface reflection at zero incidence) from IOR
    // F0 = ((ior - 1) / (ior + 1))^2
    // For plastic/glass (ior=1.5): F0 = 0.04
    // Squared by multiplication: pow is undefined for the negative base an
    // ior below 1 would produce
    float iorF0Base = (ior - 1.0) / (ior + 1.0);
    float iorF0 = iorF0Base * iorF0Base;
    vec3 F0 = vec3(iorF0);
    F0 = mix(F0, albedoMap, metallicMap);

    // Analytic keys act as small area lights (sphere-light approximation,
    // Karis 2013): widen the GGX lobe by the light's angular size and
    // scale by (a/a')^2 so reflected energy is conserved. Point lights
    // otherwise produce needle lobes on polished texels that alias into
    // per-pixel white speckle at close range — and widening WITHOUT the
    // renormalization saturates entire surfaces instead. IBL is untouched
    // and keeps the crisp environment reflections.
    float ggxAlpha = roughnessMap * roughnessMap;
    float areaAlpha = min(ggxAlpha + 0.05, 1.0);
    float areaLightNorm = (ggxAlpha / areaAlpha) * (ggxAlpha / areaAlpha);
    float areaLightRoughness = sqrt(areaAlpha);

    // Accumulate lighting from all lights
    vec3 Lo = vec3(0.0);

    // Get tangent and bitangent for anisotropy
    vec3 T = normalize(TBN[0]);
    vec3 B = normalize(TBN[1]);

    for (int i = 0; i < numLights; i++) {
        // Calculate per-light radiance
        vec3 L;
        float attenuation;

        if (lights[i].type == 0) {
            // LIGHT_DIRECTIONAL: use direction, no attenuation
            L = normalize(-lights[i].direction);
            attenuation = 1.0;
        } else {
            // Point/Spot lights: use position-based calculation
            L = normalize(lights[i].position - WorldPos);
            float distance = length(lights[i].position - WorldPos);
            attenuation = calculateAttenuation(distance, lights[i].constant,
                                               lights[i].linear, lights[i].quadratic);
        }

        // Half vector, guarded: where the light is directly behind the
        // fragment along the view ray, V + L vanishes and normalize()
        // returns garbage or NaN — and NaN * NdotL(0) stays NaN, painting
        // black flecks around the light's vanishing point. The fallback N
        // is harmless: NdotL is ~0 in that configuration anyway.
        vec3 Hraw = V + L;
        float hLen2 = dot(Hraw, Hraw);
        vec3 H = hLen2 > 1e-8 ? Hraw * inversesqrt(hLen2) : N;
        vec3 radiance = lights[i].color * lights[i].intensity * attenuation;

        // Cook-Torrance BRDF with optional anisotropy
        float NDF;
        if (anisotropyTexExists > 0 && anisotropyMap > 0.01) {
            NDF = distributionGGXAnisotropic(N, H, T, B, roughnessMap, anisotropyMap);
        } else {
            NDF = areaLightNorm * distributionGGX(N, H, areaLightRoughness);
        }
        float G = geometrySmith(N, V, L, roughnessMap);
        vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);

        // Apply thin-film interference for iridescent coatings (pilot visor style)
        if (filmThickness > 0.0) {
            float NdotV = max(dot(N, V), 0.0);
            vec3 iridescence = thinFilmInterference(filmThickness, NdotV, 1.5);
            // Strong iridescent mirror effect
            // Clamped: pow with a negative base (NdotV rounding above 1) is
            // undefined in GLSL and yields NaN on this driver
            float fresnel = pow(clamp(1.0 - NdotV, 0.0, 1.0), 2.0);
            // Replace F entirely with strong iridescent reflection
            F = iridescence * (0.6 + fresnel * 0.4);
        }

        // Specular contribution
        vec3 numerator = NDF * G * F;
        float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;
        vec3 specular = numerator / denominator;

        // Energy conservation: diffuse and specular must not exceed 1.0
        vec3 kS = F;
        vec3 kD = vec3(1.0) - kS;
        // Metals have no diffuse reflection
        kD *= 1.0 - metallicMap;

        // Lambertian diffuse
        float NdotL = max(dot(N, L), 0.0);

        // Shadow calculation for directional lights. Alpha-to-coverage
        // surfaces (hair cards) cast shadows but never receive the shadow
        // map: at map-texel scale (millimeters) card-on-card comparisons
        // are pure acne, drawing card-shaped streaks through the hair.
        // Their self-occlusion comes from the AO texture and SSAO instead.
        //
        // Each light carries its OWN shadow slot in shadowLightIndex[i];
        // sample it directly. The old getShadowSlot() reverse lookup matched
        // on light ordering, but get_closest_lights returns directional
        // lights in a per-run-variable heap order (their positions aren't
        // meaningful), so a light could sample a DIFFERENT light's shadow
        // map — a per-run-random, constant-within-a-run wrong shadow that
        // gated the specular highlights into shimmering speckle.
        float shadow = 1.0;
        if (lights[i].type == 0 && alphaToCoverage == 0 && i < MAX_SHADOW_LIGHTS) {
            int shadowSlot = shadowLightIndex[i];
            if (shadowSlot >= 0) {
                shadow = calculateShadow(shadowSlot, WorldPos, NdotL,
                                         max(lights[i].size.x, lights[i].size.y));
            }
        }

        // Add this light's contribution with shadow. Firefly clamp: a
        // sub-pixel GGX spike carries far more energy than the pixel
        // legitimately integrates, aliasing into white sparkle across
        // low-roughness normal-mapped surfaces (and before the fp16 clamp,
        // overflowing to NaN). Highlights saturate the tonemap well below
        // this cap, so only the aliasing energy is lost.
        Lo += min((kD * albedoMap / PI + specular) * radiance * NdotL * shadow, vec3(10.0));

        // Add subsurface scattering contribution
        if (subsurfaceTexExists > 0 && sssThickness < 0.99) {
            Lo += subsurfaceScattering(N, L, V, albedoMap, sssThickness, lights[i].color * lights[i].intensity * attenuation);
        }
    }

    // Ambient lighting with IBL
    vec3 ambient;
    if (iblEnabled > 0) {
        float NdotV = max(dot(N, V), 0.0);
        vec3 F = fresnelSchlickRoughness(NdotV, F0, roughnessMap);

        vec3 kS = F;
        vec3 kD = vec3(1.0) - kS;
        kD *= 1.0 - metallicMap;

        // Diffuse IBL: sample irradiance map with surface normal
        vec3 irradiance = texture(irradianceMap, N).rgb;
        vec3 diffuse = irradiance * albedoMap;

        // Specular IBL: sample prefiltered env map with reflection vector
        vec3 R = reflect(-V, N);
        vec3 prefilteredColor = textureLod(prefilteredMap, R, roughnessMap * maxReflectionLOD).rgb;
        vec2 brdf = texture(brdfLUT, vec2(NdotV, roughnessMap)).rg;
        vec3 specular = prefilteredColor * (F * brdf.x + brdf.y);

        ambient = (kD * diffuse + specular) * aoMap * iblIntensity;
    } else {
        // Fallback to simple ambient when IBL is disabled
        ambient = vec3(0.03) * albedoMap * aoMap;
    }

    // Final color, linear HDR: tone mapping and gamma happen in the post
    // pass (tonemap_frag.glsl) after MSAA resolve and bloom.
    // Clamped below half-float max (65504): the scene framebuffer is
    // RGBA16F, and a tight GGX spike under the key lights (peak ~1.2e5 at
    // the 0.04 roughness floor, times grazing Fresnel) overflows the store
    // to +INF, which the tonemap turns into NaN and displays as black
    // flecks tracing the specular highlights.
    vec3 color = min(ambient + Lo + emissiveMap, vec3(60000.0));

    // For translucent materials, apply Fresnel-based alpha
    // Edges become more reflective (less transparent) at glancing angles
    float finalOpacity = opacity;
    if (opacity < 1.0) {
        float NdotV = max(dot(N, V), 0.0);
        float fresnelOpacity = iorF0 + (1.0 - iorF0) * pow(clamp(1.0 - NdotV, 0.0, 1.0), 5.0);
        // Blend between base opacity and full opacity based on Fresnel
        finalOpacity = mix(opacity, 1.0, fresnelOpacity);
    }

    FragColor = vec4(color, finalOpacity);

    // G-buffer: view-space normal (xyz) for SSAO; alpha is a non-negative
    // reflective marker — only the shadow catcher's negative alpha traces in
    // SSR, so model surfaces stamp 0 (never reflected in screen space; they
    // rely on IBL). Alpha-to-coverage surfaces (hair) stamp zero normals
    // instead: consumers must not trust normals at strand scale, and leaving
    // the buffer unwritten is worse (stale normals of whatever drew behind
    // the hair, under the hair's depth). Their alpha must mirror FragColor's:
    // Apple's driver derives A2C coverage from the last color output's alpha,
    // not output 0, and any other value reshapes the card cutouts.
    NormalOut = alphaToCoverage > 0
                    ? vec4(0.0, 0.0, 0.0, finalOpacity)
                    : vec4(normalize(mat3(view) * N), 0.0);
}
