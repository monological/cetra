#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// 4x4 box blur matched to the SSAO noise tile period: exactly cancels the
// per-pixel rotation noise in one pass. With edgeAware on, the taps are weighted
// by linear-depth similarity (a bilateral / joint blur) so the sphere's
// silhouette occlusion cannot bleed across a depth discontinuity onto the floor
// -- the "dripping" streaks below contact shadows. Within a surface all 16 taps
// share depth, the weights are ~uniform, and it degrades to the plain box so the
// noise tile still cancels.
uniform sampler2D aoTex;
uniform sampler2D auxTex; // Aux G-buffer; .z = linear view-Z (< 0), 0 = sky
uniform vec2 texelSize;
uniform int edgeAware; // 1 = depth-bilateral; 0 = plain box (byte-identical legacy)

void main()
{
    // A weighted 4x4 box. edgeAware == 0 pins every weight to 1.0 -> the plain box
    // that cancels the noise tile, BIT-EXACT to the legacy result/16.0 (tap*1.0 is
    // an exact IEEE identity, wsum sums to exactly 16.0, and no tap-sum is
    // reordered). edgeAware == 1 down-weights each tap by a Gaussian on relative
    // linear-Z (aux .z) so the sphere's silhouette occlusion doesn't bleed across
    // the depth cliff onto the floor ("dripping"); within a surface the weights
    // are ~uniform, so the noise tile still cancels, only the cliff is rejected.
    float zc = texture(auxTex, TexCoords).z;
    float tol = max(0.05 * abs(zc), 0.02);
    float result = 0.0;
    float wsum = 0.0;
    for (int x = -2; x < 2; x++) {
        for (int y = -2; y < 2; y++) {
            vec2 offset = vec2(float(x), float(y)) * texelSize;
            float w = 1.0;
            if (edgeAware == 1) {
                float dz = (texture(auxTex, TexCoords + offset).z - zc) / tol;
                w = exp(-dz * dz);
            }
            result += texture(aoTex, TexCoords + offset).r * w;
            wsum += w;
        }
    }
    FragColor = vec4(vec3(result / wsum), 1.0);
}
