// C++ because Jolt's serialization is C++-only while its consumers are C --
// the split cluster_build.cpp precedented: one translation unit compiles
// against the library, everything else sees a C header.

#include <Jolt/Jolt.h>

#include <Jolt/Core/StreamIn.h>
#include <Jolt/Core/StreamOut.h>
#include <Jolt/Physics/Collision/PhysicsMaterial.h> // the ID maps hold Ref<PhysicsMaterial>,
                                                    // which needs the complete type
#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <cstdlib>
#include <cstring>

extern "C" {
#include "physics_cook.h"
#include "ext/log.h"
}

namespace {

// Growable-buffer streams over malloc, so the bytes handed to C are free()able
// with one copy total. StreamWrapper.h's std::iostream adapters are the
// in-library precedent and are not used: they would buy a second copy and an
// iostream dependency for two fifteen-line classes.
class ByteStreamOut final : public JPH::StreamOut {
public:
    unsigned char* data = nullptr;
    size_t size = 0;
    ~ByteStreamOut() override { free(data); } // callers steal via take()
    void WriteBytes(const void* in, size_t n) override {
        if (failed_)
            return;
        if (size + n > cap_) {
            size_t want = cap_ ? cap_ * 2 : 4096;
            while (want < size + n)
                want *= 2;
            void* grown = realloc(data, want);
            if (!grown) {
                failed_ = true;
                return;
            }
            data = static_cast<unsigned char*>(grown);
            cap_ = want;
        }
        memcpy(data + size, in, n);
        size += n;
    }
    bool IsFailed() const override { return failed_; }
    unsigned char* take() {
        unsigned char* out = data;
        data = nullptr;
        return out;
    }

private:
    size_t cap_ = 0;
    bool failed_ = false;
};

class ByteStreamIn final : public JPH::StreamIn {
public:
    ByteStreamIn(const unsigned char* data, size_t size) : data_(data), size_(size) {}
    void ReadBytes(void* out, size_t n) override {
        if (failed_ || cursor_ + n > size_) {
            failed_ = true;
            return;
        }
        memcpy(out, data_ + cursor_, n);
        cursor_ += n;
    }
    // istream semantics, and they are load-bearing: eof() goes true only after
    // a read ATTEMPTS to pass the end, never from merely standing at it. Jolt
    // checks IsEOF() right after the LAST field of a stream (Shape.cpp's
    // sRestoreWithChildren), so a positional cursor >= size here refused every
    // well-formed stream ever written -- measured as all 16 region shapes,
    // "Failed to read stream", at every size.
    bool IsEOF() const override { return failed_; }
    bool IsFailed() const override { return failed_; }

private:
    const unsigned char* data_;
    size_t size_;
    size_t cursor_ = 0;
    bool failed_ = false;
};

// JoltCImpl's OPAQUE_WRAPPER idiom (JoltC.cpp), restated locally: the two
// types are the same object seen from two languages.
inline const JPH::Shape* to_jph(const JPC_Shape* shape) {
    return reinterpret_cast<const JPH::Shape*>(shape);
}
inline JPC_Shape* to_jpc(JPH::Shape* shape) {
    return reinterpret_cast<JPC_Shape*>(shape);
}

} // namespace

extern "C" unsigned char* physics_cook_shape_serialize(const JPC_Shape* shape, size_t* out_size) {
    if (!shape || !out_size)
        return nullptr;
    ByteStreamOut out;
    JPH::Shape::ShapeToIDMap shapes;
    JPH::Shape::MaterialToIDMap materials;
    to_jph(shape)->SaveWithChildren(out, shapes, materials);
    if (out.IsFailed())
        return nullptr;
    *out_size = out.size;
    return out.take();
}

extern "C" JPC_Shape* physics_cook_shape_restore(const unsigned char* data, size_t size) {
    if (!data || !size)
        return nullptr;
    ByteStreamIn in(data, size);
    JPH::Shape::IDToShapeMap shapes;
    JPH::Shape::IDToMaterialMap materials;
    JPH::Shape::ShapeResult result = JPH::Shape::sRestoreWithChildren(in, shapes, materials);
    if (result.HasError()) {
        log_warn("physics_cook: restore refused: %s", result.GetError().c_str());
        return nullptr;
    }
    // HandleShapeResult's refcount contract (JoltCImpl/JoltC.cpp): one
    // reference handed out, released later through JPC_Shape_Release.
    JPH::Ref<JPH::Shape> ref = result.Get();
    ref->AddRef();
    return to_jpc(ref.GetPtr());
}

extern "C" uint64_t physics_cook_jolt_version(void) {
    using JPH::uint64; // JPH_VERSION_FEATURES spells the bare name
    return (uint64_t)JPH_VERSION_ID;
}
