#ifndef _JOLT_HANDLES_HPP_
#define _JOLT_HANDLES_HPP_

// What cetra's C++ files share. Each reaches a part of Jolt that JoltC binds no C for -- shape
// cooking (physics_cook.cpp), the ragdoll (ragdoll_jolt.cpp), a character's capsule changed while
// it stands (character_jolt.cpp) -- as one translation unit behind a C header, at cetra/src's root
// because the build globs C++ sources there and only C sources under game/.
//
// A JoltC handle is the Jolt object itself seen from C (JoltCImpl's OPAQUE_WRAPPER), so crossing
// between the two is a cast, stated here once for every handle these files take.

#include <Jolt/Jolt.h>

#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/PhysicsSystem.h>

typedef struct JPC_CharacterVirtual JPC_CharacterVirtual;
typedef struct JPC_PhysicsSystem JPC_PhysicsSystem;
typedef struct JPC_Shape JPC_Shape;
typedef struct JPC_TempAllocatorImpl JPC_TempAllocatorImpl;

inline JPH::CharacterVirtual* to_jph(JPC_CharacterVirtual* character) {
    return reinterpret_cast<JPH::CharacterVirtual*>(character);
}
inline JPH::PhysicsSystem* to_jph(JPC_PhysicsSystem* system) {
    return reinterpret_cast<JPH::PhysicsSystem*>(system);
}
inline const JPH::PhysicsSystem* to_jph(const JPC_PhysicsSystem* system) {
    return reinterpret_cast<const JPH::PhysicsSystem*>(system);
}
inline const JPH::Shape* to_jph(const JPC_Shape* shape) {
    return reinterpret_cast<const JPH::Shape*>(shape);
}
inline JPH::TempAllocator* to_jph(JPC_TempAllocatorImpl* allocator) {
    return reinterpret_cast<JPH::TempAllocatorImpl*>(allocator);
}
inline JPC_Shape* to_jpc(JPH::Shape* shape) {
    return reinterpret_cast<JPC_Shape*>(shape);
}

#endif // _JOLT_HANDLES_HPP_
