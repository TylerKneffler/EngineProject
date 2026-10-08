#pragma once
#include "Core/Compoonents/Obj/Mesh.h"
#include <cstdint>
#include <vector>

namespace Engine::Editor
{
struct MeshEditGeometry
{
    using Vertex = Engine::Components::Mesh::Vertex;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    static MeshEditGeometry FromMesh(const Engine::Components::Mesh& mesh);
    uint32_t FaceCount() const { return static_cast<uint32_t>(indices.size() / 3); }
    uint32_t EdgeCount() const;
    bool ExtrudeFace(uint32_t face, float distance);
    bool InsetFace(uint32_t face, float amount);
    bool SplitEdge(uint32_t edge);
    bool DeleteFace(uint32_t face);
};
}
