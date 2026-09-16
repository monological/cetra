// C++ because JPH::Ragdoll and JPH::Skeleton are C++-only while their consumers
// are C -- the split cluster_build.cpp precedented and physics_cook.cpp
// repeated: one translation unit compiles against the library, everything else
// sees a C header.

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/Skeleton/Skeleton.h>

#include <cstdlib>

extern "C" {
#include "ragdoll_jolt.h"
#include "ext/log.h"
}

namespace {

// JoltCImpl's OPAQUE_WRAPPER idiom (JoltC.cpp), restated locally: the two types
// are the same object seen from two languages. physics_cook.cpp does this for
// JPC_Shape; the physics world already holds the system handle, so nothing new
// is plumbed to reach it.
inline JPH::PhysicsSystem* to_jph(JPC_PhysicsSystem* system) {
    return reinterpret_cast<JPH::PhysicsSystem*>(system);
}

} // namespace

struct JoltRagdoll {
    JPH::Ref<JPH::RagdollSettings> settings;
    JPH::Ref<JPH::Ragdoll> ragdoll;
    // Held because Ragdoll keeps its own PhysicsSystem private with no accessor,
    // and reading a body back needs a BodyInterface.
    JPH::PhysicsSystem* system = nullptr;
};

extern "C" JoltRagdoll* jolt_ragdoll_create(JPC_PhysicsSystem* system, const RagdollBuild* parts,
                                            int count, uint32_t object_layer, uint32_t group_id) {
    if (!system || !parts || count <= 0) {
        log_error("ragdoll: nothing to build (%d part(s))", count);
        return nullptr;
    }

    // Parent-first, checked rather than trusted. JPH::Skeleton states the rule
    // (AreJointsCorrectlyOrdered) and CreateRagdoll depends on it: it indexes
    // bodies[parent] while constructing the child, so a child listed first reads
    // a slot that has not been written. Refusing here names the offender; the
    // alternative is a body built against uninitialised memory.
    for (int i = 0; i < count; i++) {
        const int parent = parts[i].parent;
        if (parent >= i || parent < -1) {
            log_error("ragdoll: part %d ('%s') has parent %d; parents must be listed first", i,
                      parts[i].name ? parts[i].name : "?", parent);
            return nullptr;
        }
        if (i > 0 && parent < 0) {
            log_error("ragdoll: part %d ('%s') is a second root; only part 0 may have none", i,
                      parts[i].name ? parts[i].name : "?");
            return nullptr;
        }
        // CapsuleShape asserts both dimensions are positive, and Jolt's default
        // Trace is JPH_ASSERT(false) -- so a zero-length bone does not produce a
        // bad ragdoll, it takes a breakpoint in a debug build. Refused here with
        // the bone's name, which is the thing the assert would not say.
        if (!(parts[i].capsule_radius > 0.0f) || !(parts[i].capsule_half_height > 0.0f)) {
            log_error("ragdoll: part %d ('%s') is degenerate: radius %.6f, half height %.6f", i,
                      parts[i].name ? parts[i].name : "?", (double)parts[i].capsule_radius,
                      (double)parts[i].capsule_half_height);
            return nullptr;
        }
    }

    // Everything is built into locals and the wrapper is allocated only once the
    // build has succeeded, so every refusal below is a scope exit and there is
    // no half-constructed object for an early return to have to unwind.
    JPH::Ref<JPH::Skeleton> skeleton = new JPH::Skeleton();
    for (int i = 0; i < count; i++) {
        const char* name = parts[i].name ? parts[i].name : "joint";
        if (parts[i].parent < 0) {
            skeleton->AddJoint(name);
        } else {
            skeleton->AddJoint(name, parts[i].parent);
        }
    }

    JPH::Ref<JPH::RagdollSettings> settings = new JPH::RagdollSettings();
    settings->mSkeleton = skeleton;
    settings->mParts.resize((size_t)count);

    for (int i = 0; i < count; i++) {
        const RagdollBuild& in = parts[i];
        JPH::RagdollSettings::Part& part = settings->mParts[(size_t)i];

        const JPH::Mat44 world =
            JPH::Mat44(JPH::Vec4(in.world[0][0], in.world[0][1], in.world[0][2], 0.0f),
                       JPH::Vec4(in.world[1][0], in.world[1][1], in.world[1][2], 0.0f),
                       JPH::Vec4(in.world[2][0], in.world[2][1], in.world[2][2], 0.0f),
                       JPH::Vec4(in.world[3][0], in.world[3][1], in.world[3][2], 1.0f));

        // A Part IS a BodyCreationSettings, plus the one constraint field below.
        part.SetShape(new JPH::CapsuleShape(in.capsule_half_height, in.capsule_radius));
        part.mPosition = JPH::RVec3(world.GetTranslation());
        part.mRotation = world.GetQuaternion().Normalized();
        part.mMotionType = JPH::EMotionType::Dynamic;
        part.mObjectLayer = (JPH::ObjectLayer)object_layer;

        if (in.parent >= 0) {
            // SwingTwist and not one of the other eleven constraint types: Jolt
            // documents it as the specialized humanoid-ragdoll joint, and it is
            // the ONLY type DriveToPoseUsingMotors accepts -- every other one
            // trips an assert there. Nothing here drives to a pose, but choosing
            // it now is what leaves a getting-up spec possible later.
            JPH::SwingTwistConstraintSettings* joint = new JPH::SwingTwistConstraintSettings();
            // The limb runs along its own local Y, which is also the twist axis.
            const JPH::Vec3 along = world.GetAxisY().Normalized();
            /*
             * The anchor is the limb's HEAD, not its centre, and the difference
             * is the whole difference between a body and a heap.
             *
             * `world` places the MIDDLE of the capsule, because that is where a
             * CapsuleShape's origin is; the joint this constraint stands for is
             * at the end of it, half a limb back along the twist axis. Anchored
             * at the centre the constraint is still perfectly rigid -- it just
             * pivots the limb about its own middle, so a shoulder rotating 90
             * degrees swings the arm's head half an arm away from the torso and
             * the character comes apart into a cloud of limbs that never stop
             * being constrained. It settles, it frees cleanly, every arm in the
             * ragdoll group stays green, and it looks like an explosion.
             *
             * Half the limb is `half_height + radius`: the half height excludes
             * the two caps, and the caps are what the rest of the length is.
             */
            const JPH::RVec3 anchor = JPH::RVec3(
                world.GetTranslation() - along * (in.capsule_half_height + in.capsule_radius));
            joint->mPosition1 = joint->mPosition2 = anchor;
            joint->mTwistAxis1 = joint->mTwistAxis2 = along;
            joint->mPlaneAxis1 = joint->mPlaneAxis2 = world.GetAxisZ().Normalized();
            joint->mNormalHalfConeAngle = JPH::DegreesToRadians(in.cone_deg);
            joint->mPlaneHalfConeAngle = JPH::DegreesToRadians(in.plane_deg);
            joint->mTwistMinAngle = JPH::DegreesToRadians(in.twist_min_deg);
            joint->mTwistMaxAngle = JPH::DegreesToRadians(in.twist_max_deg);
            part.mToParent = joint;
        }
    }

    // Parents heavier than their children, so a light forearm cannot throw the
    // torso around. Jolt's own sample calls this optional; it is not optional on
    // a rig whose capsule masses come from measured limb volumes, where a head
    // and a thigh can differ by an order of magnitude.
    settings->Stabilize();
    /*
     * The collision group that stops a limb colliding with what it is bolted
     * to. Without it every constraint fights a contact at its own joint.
     *
     * It takes an optional POSE, which makes it disable every pair already
     * interpenetrating rather than only parent and child -- and on capsules
     * measured from a rig, sibling thighs both start inside the pelvis, so
     * that looked like the fix for a character that came apart at the hips.
     * It is not: passing it measured bit-identical, and the pairs were never
     * the problem. Left off rather than kept as a plausible-sounding no-op.
     */
    settings->DisableParentChildCollisions();
    settings->CalculateBodyIndexToConstraintIndex();

    JPH::Ref<JPH::Ragdoll> built =
        settings->CreateRagdoll((JPH::CollisionGroup::GroupID)group_id, 0, to_jph(system));
    if (built == nullptr) {
        // CreateRagdoll's only documented failure: the body pool is full.
        log_error("ragdoll: out of bodies building %d part(s)", count);
        return nullptr;
    }

    JoltRagdoll* out = new (std::nothrow) JoltRagdoll();
    if (!out) {
        // The bodies exist but were never added, so releasing is the whole
        // cleanup -- ~Ragdoll destroys them and there is nothing to remove.
        return nullptr;
    }
    out->settings = settings;
    out->ragdoll = built;
    out->system = to_jph(system);
    out->ragdoll->AddToPhysicsSystem(JPH::EActivation::Activate);
    return out;
}

extern "C" void jolt_ragdoll_destroy(JoltRagdoll* ragdoll) {
    if (!ragdoll) {
        return;
    }
    if (ragdoll->ragdoll != nullptr) {
        // Removed BEFORE the reference goes. ~Ragdoll destroys its bodies and
        // does not remove them, so dropping the last reference while they are
        // still added destroys bodies the broadphase is still indexing.
        ragdoll->ragdoll->RemoveFromPhysicsSystem();
    }
    delete ragdoll; // the Refs release here, and ~Ragdoll destroys the bodies
}

extern "C" bool jolt_ragdoll_get_world(const JoltRagdoll* ragdoll, int index, mat4 out) {
    if (!ragdoll || ragdoll->ragdoll == nullptr || !ragdoll->system || !out || index < 0 ||
        index >= (int)ragdoll->ragdoll->GetBodyCount()) {
        return false;
    }
    const JPH::BodyID id = ragdoll->ragdoll->GetBodyID(index);
    const JPH::BodyInterface& bi = ragdoll->system->GetBodyInterface();
    const JPH::RMat44 m = bi.GetWorldTransform(id);
    for (int c = 0; c < 4; c++) {
        const JPH::Vec4 col = JPH::Vec4(m.GetColumn4(c));
        out[c][0] = col.GetX();
        out[c][1] = col.GetY();
        out[c][2] = col.GetZ();
        out[c][3] = col.GetW();
    }
    return true;
}

extern "C" int jolt_ragdoll_world_body_count(const JPC_PhysicsSystem* system) {
    if (!system) {
        return 0;
    }
    return (int)reinterpret_cast<const JPH::PhysicsSystem*>(system)->GetNumBodies();
}
