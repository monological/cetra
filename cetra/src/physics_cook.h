#ifndef _PHYSICS_COOK_H_
#define _PHYSICS_COOK_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Jolt shape serialization for the derived-data cook (spec 11.99). C++ behind
 * a C header because Jolt's serialization surface -- Shape::SaveWithChildren
 * and sRestoreWithChildren -- exists only on the C++ side and JoltC binds
 * none of it; the same one-TU split cluster_build.cpp precedented and JoltC
 * itself uses. Lives in cetra/src/ root because the build globs C++ sources
 * at the src root and only C sources under game/, so a .cpp under game/
 * would silently not compile.
 */

typedef struct JPC_Shape JPC_Shape; // matches JoltC/Functions.h's opaque type

// Serialize a built shape -- BVH and all -- to a malloc'd buffer the caller
// frees. SaveWithChildren, so materials and sub-shapes ride the one stream
// the day a compound shape is cooked. NULL on failure.
unsigned char* physics_cook_shape_serialize(const JPC_Shape* shape, size_t* out_size);

// Restore. Hands the caller ONE reference, the same contract
// physics_create_mesh_shape's return carries; release with JPC_Shape_Release.
// NULL on any failure -- truncated, malformed, wrong Jolt version -- and the
// caller builds live.
JPC_Shape* physics_cook_shape_restore(const unsigned char* data, size_t size);

// JPH_VERSION_ID, evaluated where the C++ headers are visible. The mandatory
// key axis: Jolt documents its cooked format as not backward compatible
// across library versions (Shape.h's SaveBinaryState note), so this folds
// into every cooked-shape key and a Jolt upgrade orphans the artefacts
// instead of feeding them to a reader that misparses. The ID carries the
// feature bits too, so debug and release address separate entries -- paid
// deliberately: conservative over clever for a format the library owns.
uint64_t physics_cook_jolt_version(void);

#ifdef __cplusplus
}
#endif

#endif // _PHYSICS_COOK_H_
