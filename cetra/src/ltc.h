#ifndef LTC_H
#define LTC_H

#include <GL/glew.h>

#include "program.h"

// Linearly Transformed Cosines lookup tables (Heitz, Dupuy, Hill, Neubelt,
// SIGGRAPH 2016) -- the fitted data behind rectangular area lights. Spec 9.2.
//
// Engine-lifetime rather than scene-lifetime: unlike the IBL/probe textures
// these are static fitted constants with no dependence on scene content, so
// they are uploaded once at init and shared by every scene. The table data
// itself is generated (cetra/src/ltc_lut.h, from tools/gen_ltc_lut.py) and is
// deliberately included only by ltc.c -- it is half a megabyte of static
// arrays that nothing else needs in its translation unit.
typedef struct LTCTables {
    GLuint tex; // 2-layer array: 0 inverse-M fit, 1 magnitude/Fresnel + .w (TEXUNIT_LTC)
} LTCTables;

// Uploads both tables. Returns NULL if the texture could not be created.
LTCTables* create_ltc_tables(void);
void free_ltc_tables(LTCTables* ltc);

// Bind the table array and point its sampler at the reserved unit. Safe to
// call for any program: one without the sampler no-ops on the uniform lookup,
// and the shader only reads it when the scene actually has panels.
void bind_ltc_tables(const LTCTables* ltc, ShaderProgram* program);

#endif // LTC_H
