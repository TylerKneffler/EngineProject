#pragma once

#include "Core/Component.h"
#include <cstdint>
#include <glm/glm.hpp>
#include <string>
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
    // PerBone uses collider components referenced by IKBone. WholeMesh uses
    // one MeshObjectCollider on the skeleton root's RigidBody.
    std::string colliderMode = "PerBone";
    ComponentReference meshColliderReference { "MeshObjectCollider" };
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
    bool UsesWholeMeshCollider() const;

private:
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
