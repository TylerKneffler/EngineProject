#pragma once

#include "Core/Component.h"
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

namespace Engine::Components
{
class Skeleton : public Engine::Core::Component
{
public:
    Skeleton();
    ComponentReference modelReference { "Model" };
    bool showBones = true;
    unsigned skinIndex = 0;
    std::vector<unsigned> jointNodes;
    std::vector<glm::mat4> inverseBindMatrices;
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;
    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;
    Object* GetHierarchyRoot() const;
    const std::vector<Object*>& ResolveJoints() const;
    Object* FindNode(unsigned index) const;
    class Model* ResolveModel() const;

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
};
}
