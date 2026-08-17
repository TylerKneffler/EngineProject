#pragma once

#include "Core/Component.h"
#include "Core/Compoonents/Mesh.h"
#include <glm/glm.hpp>
#include <vector>

namespace Engine::Components
{
class SkinnedMesh : public Engine::Core::Component
{
public:
    using Vertex = Engine::Model::Vertex;

    SkinnedMesh();
    ComponentReference meshReference { "Mesh" };
    ComponentReference skeletonReference { "Skeleton" };
    int skinIndex = -1;
    std::vector<glm::uvec4> joints;
    std::vector<glm::vec4> weights;
    void Start() override;
    void Update() override;
    void OnAfterDeserialize(IGraphicsProvider*) override { Start(); }
    bool BuildPalette(std::vector<glm::mat4>& palette) const;
    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;

private:
    std::vector<Vertex> m_baseVertices;
};
}
