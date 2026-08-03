#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// Half-res screen march of the cloud shell (spec 11.0). Rays reconstruct
// from the UNJITTERED projection (the aerial/froxel discipline); output is
// (absolute in-scattered radiance, transmittance) -- the background
// composite applies the exposure conversion and the geometry depth test.

uniform sampler3D shapeTex;
uniform sampler3D detailTex;
uniform sampler2D transmittanceLut;
uniform sampler2D skyViewLut;
uniform mat4 invView;  // view -> world
uniform vec2 invFocal; // (1/proj[0][0], 1/proj[1][1])
uniform vec3 camPosKm; // world camera position / unitsPerKm
uniform vec3 sunDir;
uniform float coverage;
uniform float cloudType;
uniform float densityScale;
uniform int steps;
uniform int lightSteps;
uniform int debugShell; // 1 = shell entry/exit distances as color (R5 probe)

#include "noise.glsl"
#include "clouds.glsl"

void main()
{
    vec2 ndc = TexCoords * 2.0 - 1.0;
    vec3 viewDir = normalize(vec3(ndc * invFocal, -1.0));
    vec3 rd = normalize(mat3(invView) * viewDir);

    if (debugShell == 1) {
        // R5 probe: entry distance (R, /64 km) and marched span (G, /48 km)
        // of the shell intersection, to expose grazing-incidence banding.
        float obsAlt = max(camPosKm.y, VIEW_ALTITUDE);
        vec3 centre = vec3(camPosKm.x, camPosKm.y - (Rg + obsAlt), camPosKm.z);
        float tIn = cloudSphereNear(camPosKm, rd, centre, Rg + CLOUD_BOTTOM_KM);
        float tOut = cloudSphereNear(camPosKm, rd, centre, Rg + CLOUD_TOP_KM);
        float span = (tOut > 0.0) ? tOut - max(tIn, 0.0) : 0.0;
        FragColor = vec4(max(tIn, 0.0) / 64.0, span / 48.0, 0.0, 1.0);
        return;
    }

    float dither = ign(gl_FragCoord.xy);

    FragColor = cloud_march(camPosKm, rd, sunDir, shapeTex, detailTex, transmittanceLut,
                            skyViewLut, steps, lightSteps, true, coverage, cloudType,
                            densityScale, vec3(0.0), dither);
}
