#include "ltc.h"

#include <stdlib.h>

#include "common.h"
#include "ext/log.h"
#include "ltc_lut.h" // generated LTC tables (spec 9.2); see tools/gen_ltc_lut.py
#include "uniform.h"

LTCTables* create_ltc_tables(void) {
    LTCTables* ltc = calloc(1, sizeof(LTCTables));
    if (!ltc) {
        log_error("Failed to allocate LTC tables");
        return NULL;
    }

    // One 2-layer array rather than two 2D textures: the tables always bind
    // together, and packing them frees a fragment sampler unit in a program
    // where all 16 are spoken for. RGBA32F because the inverse-M entries
    // leave [0,1] and the published fit is float32; 128 KB for both. Same
    // data-LUT policy as create_texture_2d_float: LINEAR, CLAMP_TO_EDGE,
    // no mips.
    glGenTextures(1, &ltc->tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, ltc->tex);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA32F, LTC_LUT_DIM, LTC_LUT_DIM, 2, 0, GL_RGBA,
                 GL_FLOAT, NULL);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, LTC_LUT_DIM, LTC_LUT_DIM, 1, GL_RGBA,
                    GL_FLOAT, LTC_MAT_TABLE);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 1, LTC_LUT_DIM, LTC_LUT_DIM, 1, GL_RGBA,
                    GL_FLOAT, LTC_AMP_TABLE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    return ltc;
}

void free_ltc_tables(LTCTables* ltc) {
    if (!ltc)
        return;
    if (ltc->tex)
        glDeleteTextures(1, &ltc->tex);
    free(ltc);
}

void bind_ltc_tables(const LTCTables* ltc, ShaderProgram* program) {
    if (!ltc || !program || !program->uniforms)
        return;

    UniformManager* u = program->uniforms;

    glActiveTexture(GL_TEXTURE0 + TEXUNIT_LTC);
    glBindTexture(GL_TEXTURE_2D_ARRAY, ltc->tex);
    uniform_set_int(u, "ltcTex", TEXUNIT_LTC);

    glActiveTexture(GL_TEXTURE0);
}
