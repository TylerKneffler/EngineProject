#pragma once

#include "Core/Component.h"
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine::Components
{
class AnimationBone;
class MeshObjectCollider;
class Skeleton : public Engine::Core::Component
{
public:
    Skeleton();
    ComponentReference modelReference { "Model" };
    // PerBone uses IKBone colliders. MeshCollider uses one root collider.
    // SkinnedBoneHulls uses IKBone colliders fitted to skin-weighted mesh.
    std::string colliderMode = "PerBone";
    ComponentReference meshColliderReference { "MeshObjectCollider" };
    // Add a bounded pose response to mapped contacts on a mesh-collider body.
    // The root rigid body remains responsible for world motion.
    bool meshContactResponseEnabled = true;
    float meshContactResponseStrength = 8.f;
    float meshContactDamping = 7.f;
    float meshContactMaxBend = 0.35f;
    float meshImpactImpulseThreshold = 0.5f;
    float meshIKGroundedGravityScale = 0.02f;
    float meshIKTorqueLimitPerMass = 0.015f;
    // Fallback pose solver for joints without active pose-only IK bodies.
    // The root body owns translation and floor response; bones only rotate.
    bool meshRagdollEnabled = false;
    float meshRagdollGravityStrength = 3.f;
    bool showBones = true;
    unsigned skinIndex = 0;
    std::vector<unsigned> jointNodes;
    std::vector<glm::mat4> inverseBindMatrices;
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;
    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;
    Object* GetHierarchyRoot() const;
    const std::vector<Object*>& ResolveJoints() const;
    // AnimationBone components are returned in skinning-palette order. Root bones are
    // returned in object-hierarchy order and are independent of palette order.
    const std::vector<AnimationBone*>& ResolveBones() const;
    const std::vector<AnimationBone*>& ResolveRootBones() const;
    Object* FindNode(unsigned index) const;
    class Model* ResolveModel() const;
    MeshObjectCollider* ResolveMeshCollider() const;
    bool UsesMeshCollider() const;
    void ApplyMeshContactResponse(float stepSeconds);
    void ResetMeshContactResponse();
    uint64_t GetMeshIKTorqueEvents() const { return m_meshIKTorqueEvents; }

private:
    struct ContactBoneState
    {
        glm::quat baseRotation { 1.f, 0.f, 0.f, 0.f };
        glm::quat lastAppliedRotation { 1.f, 0.f, 0.f, 0.f };
        glm::vec3 offset { 0.f };
        glm::vec3 angularVelocity { 0.f };
        bool hasAppliedPose = false;
    };
    std::unordered_map<AnimationBone*, ContactBoneState> m_contactBoneStates;
    uint64_t m_meshIKTorqueEvents = 0;
    uint64_t JointBindingSignature() const;
    mutable Model* m_cachedModel = nullptr;
    mutable std::vector<Object*> m_cachedJoints;
    mutable uint64_t m_cachedStructureRevision = 0;
    mutable uint64_t m_cachedConfigurationRevision = 0;
    mutable uint64_t m_cachedModelRevision = 0;
    mutable uint64_t m_cachedJointSignature = 0;
    mutable uint64_t m_cachedJointStructureRevision = 0;
    mutable Model* m_cachedJointModel = nullptr;
    mutable bool m_modelCacheValid = false;
    mutable std::vector<AnimationBone*> m_cachedBones;
    mutable std::vector<AnimationBone*> m_cachedRootBones;
};
}
