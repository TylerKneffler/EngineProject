#pragma once
#include "Core/Compoonents/Obj/Mesh.h"
#include <cstdint>
#include <array>
#include <utility>
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
    std::vector<std::pair<uint32_t, uint32_t>> EdgeList() const;
    static std::vector<std::pair<uint32_t, uint32_t>> EdgeListFromIndices(
        const std::vector<uint32_t>& indices);
    std::pair<uint32_t, uint32_t> EdgeAt(uint32_t edge) const;
    std::vector<uint32_t> SelectionVertices(int mode,
        const std::vector<uint32_t>& selected) const;
    bool TransformSelection(int mode, const std::vector<uint32_t>& selected,
        const std::array<float, 3>& translation,
        const std::array<float, 3>& rotation,
        const std::array<float, 3>& scale, int pivotMode,
        const std::array<float, 3>& customPivot, float snapStep,
        float rotationSnap = 0.f, float scaleSnap = 0.f);
    bool ExtrudeFace(uint32_t face, float distance);
    bool InsetFace(uint32_t face, float amount);
    bool SplitEdge(uint32_t edge);
    bool DeleteFace(uint32_t face);
    bool DeleteSelection(int mode, const std::vector<uint32_t>& selected);
    bool DuplicateFaces(const std::vector<uint32_t>& faces,
        const std::array<float, 3>& offset);
    bool WeldVertices(const std::vector<uint32_t>& selected);
    bool FillBoundary(const std::vector<uint32_t>& selectedEdges);
    bool BridgeBoundaries(const std::vector<uint32_t>& selectedEdges);
    bool BevelEdge(uint32_t edge, float width);
    bool LoopCut(uint32_t edge);
};
}
