#ifndef _CHARACTER_JOLT_H_
#define _CHARACTER_JOLT_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A character's shape changed while it stands in the world (spec 13.48), behind a C header. The
 * fourth one-TU split, after cluster_build.cpp, physics_cook.cpp and ragdoll_jolt.cpp, and for the
 * same reason: CharacterVirtual::SetShape and RotatedTranslatedShape are compiled into this binary
 * and JoltC binds neither. Lives in cetra/src/ root because the build globs C++ sources there and
 * only C sources under game/.
 */

typedef struct JPC_CharacterVirtual JPC_CharacterVirtual;   // JoltC/JoltCharacter.h's opaque type
typedef struct JPC_PhysicsSystem JPC_PhysicsSystem;         // JoltC/Functions.h's
typedef struct JPC_TempAllocatorImpl JPC_TempAllocatorImpl; // JoltC/Functions.h's

// Gives the character, and its inner body, a capsule of `radius` and cylinder `half_height` whose
// centre is `offset_y` above the character's position, colliding as `object_layer` does. With
// `check`, a capsule that would sink into what is round it deeper than Jolt lets bodies rest in
// contact is refused: false, and the shape is left as it was.
bool jolt_character_set_capsule(JPC_CharacterVirtual* character, JPC_PhysicsSystem* system,
                                JPC_TempAllocatorImpl* allocator, uint32_t object_layer,
                                float radius, float half_height, float offset_y, bool check);

#ifdef __cplusplus
}
#endif

#endif // _CHARACTER_JOLT_H_
