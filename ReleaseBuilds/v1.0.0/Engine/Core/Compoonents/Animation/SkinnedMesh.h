#pragma once

#include "Core/Component.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

namespace Engine::Components
{
class Mesh;
class Skeleton;
class SkinnedMesh : public Engine::Core::Component
{
public:
    using Vertex = Engine::Model::AnimationVertex;

    SkinnedMesh();
    ComponentReference meshReference { "Mesh" };
    ComponentReference skeletonReference { "Skeleton" };
    int skinIndex = -1;
    std::vector<glm::uvec4> joints;
    std::vector<glm::vec4> weights;
    void Start() override;
    void Update() override;
    void OnAfterDeserialize(IGraphicsProvider*) override { Start(); }
    const std::vector<glm::mat4>& BuildPalette() const;
    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;

private:
    void ResolveBindings() const;
    mutable Mesh* m_cachedMesh = nullptr;
    mutable Skeleton* m_cachedSkeleton = nullptr;
    mutable uint64_t m_cachedStructureRevision = 0;
    mutable uint64_t m_cachedConfigurationRevision = 0;
    mutable int m_cachedSkinIndex = -2;
    mutable bool m_bindingCacheValid = false;
    mutable std::vector<glm::mat4> m_palette;
};
}
