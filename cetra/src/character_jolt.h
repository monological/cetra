#ifndef _CHARACTER_JOLT_H_
#define _CHARACTER_JOLT_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A character controller's capsule, built and changed while it stands (spec 13.48), and what of it
 * can stand on anything, behind a C header: JoltC binds neither RotatedTranslatedShape,
 * CharacterVirtual::SetShape nor the settings' supporting volume. jolt_handles.hpp says why this is
 * C++ at cetra/src's root.
 */

typedef struct JPC_CharacterVirtual JPC_CharacterVirtual; // JoltC/JoltCharacter.h's opaque type
typedef struct JPC_CharacterVirtualSettings JPC_CharacterVirtualSettings; // and its settings'
typedef struct JPC_PhysicsSystem JPC_PhysicsSystem;                       // JoltC/Functions.h's
typedef struct JPC_Shape JPC_Shape;                                       // JoltC/Functions.h's
typedef struct JPC_TempAllocatorImpl JPC_TempAllocatorImpl;               // JoltC/Functions.h's

// A capsule of `radius` and cylinder `half_height` whose centre is `offset_y` above the point it
// is placed at; at an offset of 0, a plain capsule. One reference, the caller's to release with
// JPC_Shape_Release; NULL, said by name, for a capsule with no size.
JPC_Shape* jolt_character_capsule(float radius, float half_height, float offset_y);

// Gives the character, and its inner body, `shape`, colliding as `object_layer` does. With
// `check`, a shape that would sink into what is round it deeper than Jolt lets bodies rest in
// contact is refused, false and nothing changed. What is round it is Jolt's own contacts: a body
// the character's listener lets it pass through still counts.
bool jolt_character_set_shape(JPC_CharacterVirtual* character, JPC_PhysicsSystem* system,
                              JPC_TempAllocatorImpl* allocator, uint32_t object_layer,
                              const JPC_Shape* shape, bool check);

// A character made from `settings` stands only on what it touches below `y`, in its own frame;
// whatever it touches above only blocks it.
void jolt_character_support_below(JPC_CharacterVirtualSettings* settings, float y);

#ifdef __cplusplus
}
#endif

#endif // _CHARACTER_JOLT_H_
