#pragma once

#include "Core/Component.h"
#include "Core/Compoonents/Mesh.h"
#include <glm/glm.hpp>
#include <vector>

class SkinnedMesh : public Component
{
public:
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
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;

private:
    std::vector<Vertex> m_baseVertices;
};
