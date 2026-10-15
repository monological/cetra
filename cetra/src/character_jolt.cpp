#include <Jolt/Jolt.h>

#include "jolt_handles.hpp"

#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>

#include <cfloat>

extern "C" {
#include "character_jolt.h"
#include "ext/log.h"
}

extern "C" JPC_Shape* jolt_character_capsule(float radius, float half_height, float offset_y) {
    // Through the settings, which refuse a degenerate capsule by name where CapsuleShape's
    // constructor asserts, and Jolt's default assert is a breakpoint.
    JPH::Ref<JPH::ShapeSettings> settings = new JPH::CapsuleShapeSettings(half_height, radius);
    if (offset_y != 0.0f)
        settings = new JPH::RotatedTranslatedShapeSettings(JPH::Vec3(0.0f, offset_y, 0.0f),
                                                           JPH::Quat::sIdentity(), settings);
    JPH::ShapeSettings::ShapeResult made = settings->Create();
    if (made.HasError()) {
        log_error("character: no capsule %.3f high, %.3f round: %s", (double)half_height,
                  (double)radius, made.GetError().c_str());
        return nullptr;
    }
    JPH::Shape* shape = made.Get().GetPtr();
    shape->AddRef();
    return to_jpc(shape);
}

extern "C" bool jolt_character_set_shape(JPC_CharacterVirtual* character, JPC_PhysicsSystem* system,
                                         JPC_TempAllocatorImpl* allocator, uint32_t object_layer,
                                         const JPC_Shape* shape, bool check) {
    if (!character || !system || !allocator || !shape)
        return false;
    JPH::PhysicsSystem* jph_system = to_jph(system);
    const JPH::ObjectLayer layer = (JPH::ObjectLayer)object_layer;
    // Jolt's own crouch sample allows the new shape half again the penetration bodies are left
    // resting at; anything deeper is something in the way.
    const float most = check ? 1.5f * jph_system->GetPhysicsSettings().mPenetrationSlop : FLT_MAX;
    JPH::CharacterVirtual* jph_character = to_jph(character);
    if (!jph_character->SetShape(
            to_jph(shape), most, jph_system->GetDefaultBroadPhaseLayerFilter(layer),
            jph_system->GetDefaultLayerFilter(layer), {}, {}, *to_jph(allocator)))
        return false;
    jph_character->SetInnerBodyShape(to_jph(shape));
    return true;
}

extern "C" void jolt_character_support_below(JPC_CharacterVirtualSettings* settings, float y) {
    // A contact's signed distance from the plane is its height over `y`; behind it, it can support.
    if (settings)
        to_jph(settings)->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -y);
}
