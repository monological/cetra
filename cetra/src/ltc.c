#include "ltc.h"

#include <stdlib.h>

#include "common.h"
#include "ext/log.h"
#include "ltc_lut.h" // generated LTC tables (spec 9.2); see tools/gen_ltc_lut.py
#include "texture.h"
#include "uniform.h"

LTCTables* create_ltc_tables(void) {
    LTCTables* ltc = calloc(1, sizeof(LTCTables));
    if (!ltc) {
        log_error("Failed to allocate LTC tables");
        return NULL;
    }

    // RGBA32F because the inverse-M entries leave [0,1] and the published fit
    // is float32; 128 KB for both.
    ltc->mat_tex =
        create_texture_2d_float(LTC_LUT_DIM, LTC_LUT_DIM, GL_RGBA32F, GL_RGBA, LTC_MAT_TABLE);
    ltc->amp_tex =
        create_texture_2d_float(LTC_LUT_DIM, LTC_LUT_DIM, GL_RGBA32F, GL_RGBA, LTC_AMP_TABLE);
    if (!ltc->mat_tex || !ltc->amp_tex) {
        log_error("Failed to create LTC area-light lookup tables");
        free_ltc_tables(ltc);
        return NULL;
    }
    return ltc;
}

void free_ltc_tables(LTCTables* ltc) {
    if (!ltc)
        return;
    if (ltc->mat_tex)
        glDeleteTextures(1, &ltc->mat_tex);
    if (ltc->amp_tex)
        glDeleteTextures(1, &ltc->amp_tex);
    free(ltc);
}

void bind_ltc_tables(const LTCTables* ltc, ShaderProgram* program) {
    if (!ltc || !program || !program->uniforms)
        return;

    UniformManager* u = program->uniforms;

    glActiveTexture(GL_TEXTURE0 + TEXUNIT_LTC_MAT);
    glBindTexture(GL_TEXTURE_2D, ltc->mat_tex);
    uniform_set_int(u, "ltcMatTex", TEXUNIT_LTC_MAT);

    glActiveTexture(GL_TEXTURE0 + TEXUNIT_LTC_AMP);
    glBindTexture(GL_TEXTURE_2D, ltc->amp_tex);
    uniform_set_int(u, "ltcAmpTex", TEXUNIT_LTC_AMP);

    glActiveTexture(GL_TEXTURE0);
}
