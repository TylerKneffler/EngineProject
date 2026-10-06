#pragma once

#include "Core/Compoonents/Physics/Collider.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Engine::Physics { class Physics; }
namespace Engine::Components { class AnimationBone; class RigidBody; }

namespace Engine::Components
{
class MeshObjectCollider final : public Collider
{
public:
    struct BoneWeight
    {
        unsigned paletteIndex = 0;
        float weight = 0.f;
        AnimationBone* bone = nullptr;
    };

    struct Contact
    {
        // Valid until the next physics step. Normal points from the other
        // body toward this collider; impulse is the solver's normal impulse.
        const RigidBody* otherBody = nullptr;
        glm::vec3 pointWorld { 0.f };
        glm::vec3 normalWorld { 0.f };
        float separation = 0.f;
        float normalImpulse = 0.f;

        // Closest point on the current morphed and skinned triangle stream.
        // A convex-hull contact can be too far away to map reliably.
        bool surfaceMapped = false;
        glm::vec3 surfacePointWorld { 0.f };
        float surfaceDistance = 0.f;
        uint32_t triangleIndex = 0;
        glm::vec3 barycentric { 0.f };
        std::array<BoneWeight, 24> boneWeights {};
        uint8_t boneWeightCount = 0;

        float WeightForPaletteIndex(unsigned index) const;
    };

    MeshObjectCollider();

    PROPERTY(Inspector, EditAnywhere, Category = "Collider")
    ComponentReference meshReference { "Mesh" };
    PROPERTY(Inspector, EditAnywhere, Category = "Collider")
    std::string meshPath;
    // Dynamic skinned meshes use a convex hull of the current pose.
    PROPERTY(Inspector, EditAnywhere, Category = "Collider")
    bool convex = true;
    PROPERTY(Inspector, EditAnywhere, Category = "Collider | Contacts", ClampMin = "0")
    float maxSurfaceMappingDistance = 0.3f;

    // Snapshot from the latest Physics::Step. Consumers can read it during
    // the next scene Update; it is replaced by the following physics step.
    const std::vector<Contact>& GetContacts() const { return m_contacts; }
    uint64_t GetContactRevision() const { return m_contactRevision; }

private:
    friend class Engine::Physics::Physics;
    bool IsActiveForBody(const RigidBody* body) const;
    void BeginContactFrame();
    void RecordContact(const RigidBody* other, const glm::vec3& point,
        const glm::vec3& normal, float separation, float normalImpulse);
    void ResolveContactSurfaces();

    std::vector<Contact> m_contacts;
    uint64_t m_contactRevision = 0;
};
}
