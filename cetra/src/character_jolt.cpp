// C++ because CharacterVirtual::SetShape and RotatedTranslatedShape are C++-only while their
// consumer is C -- the split ragdoll_jolt.cpp repeated from cluster_build.cpp and physics_cook.cpp.

#include <Jolt/Jolt.h>

#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <cfloat>

extern "C" {
#include "character_jolt.h"
#include "ext/log.h"
}

namespace {

// JoltCImpl's OPAQUE_WRAPPER idiom (JoltCharacter.cpp), restated locally as ragdoll_jolt.cpp
// does: each pair is the same object seen from two languages.
inline JPH::CharacterVirtual* to_jph(JPC_CharacterVirtual* character) {
    return reinterpret_cast<JPH::CharacterVirtual*>(character);
}
inline JPH::PhysicsSystem* to_jph(JPC_PhysicsSystem* system) {
    return reinterpret_cast<JPH::PhysicsSystem*>(system);
}
inline JPH::TempAllocator* to_jph(JPC_TempAllocatorImpl* allocator) {
    return reinterpret_cast<JPH::TempAllocatorImpl*>(allocator);
}

} // namespace

extern "C" bool jolt_character_set_capsule(JPC_CharacterVirtual* character,
                                           JPC_PhysicsSystem* system,
                                           JPC_TempAllocatorImpl* allocator, uint32_t object_layer,
                                           float radius, float half_height, float offset_y,
                                           bool check) {
    if (!character || !system || !allocator)
        return false;
    // Through the settings, which refuse a degenerate capsule by name where CapsuleShape's
    // constructor asserts, and Jolt's default assert is a breakpoint.
    JPH::RotatedTranslatedShapeSettings settings(
        JPH::Vec3(0.0f, offset_y, 0.0f), JPH::Quat::sIdentity(),
        new JPH::CapsuleShapeSettings(half_height, radius));
    JPH::ShapeSettings::ShapeResult made = settings.Create();
    if (made.HasError()) {
        log_error("character: no capsule %.3f high, %.3f round: %s", (double)half_height,
                  (double)radius, made.GetError().c_str());
        return false;
    }
    const JPH::RefConst<JPH::Shape> shape = made.Get();
    JPH::PhysicsSystem* jph_system = to_jph(system);
    const JPH::ObjectLayer layer = (JPH::ObjectLayer)object_layer;
    // Jolt's own crouch sample allows the new shape half again the penetration bodies are left
    // resting at; anything deeper is something in the way.
    const float most = check ? 1.5f * jph_system->GetPhysicsSettings().mPenetrationSlop : FLT_MAX;
    JPH::CharacterVirtual* jph_character = to_jph(character);
    if (!jph_character->SetShape(shape, most, jph_system->GetDefaultBroadPhaseLayerFilter(layer),
                                 jph_system->GetDefaultLayerFilter(layer), {}, {},
                                 *to_jph(allocator)))
        return false;
    jph_character->SetInnerBodyShape(shape);
    return true;
}
