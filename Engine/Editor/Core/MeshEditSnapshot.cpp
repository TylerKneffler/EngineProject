#include "MeshEditSnapshot.h"
#include "SnapshotHistory.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include <cstdint>
#include <cstring>
#include <vector>

namespace Engine::Editor
{
std::string CaptureMeshSnapshot(const Engine::Components::Mesh& mesh)
{
    const auto& vertices = mesh.GetVertices();
    const auto& indices = mesh.GetIndices();
    const uint32_t counts[]{ static_cast<uint32_t>(vertices.size()),
        static_cast<uint32_t>(indices.size()) };
    std::string result(reinterpret_cast<const char*>(counts), sizeof(counts));
    result.append(reinterpret_cast<const char*>(vertices.data()),
        vertices.size() * sizeof(Engine::Components::Mesh::Vertex));
    result.append(reinterpret_cast<const char*>(indices.data()),
        indices.size() * sizeof(uint32_t));
    return result;
}

bool RestoreMeshSnapshot(Engine::Components::Mesh& mesh,
    const std::string& snapshot)
{
    using Vertex = Engine::Components::Mesh::Vertex;
    if (snapshot.size() < sizeof(uint32_t) * 2) return false;
    uint32_t counts[2]{};
    std::memcpy(counts, snapshot.data(), sizeof(counts));
    const size_t vertexBytes = static_cast<size_t>(counts[0]) * sizeof(Vertex);
    const size_t indexBytes = static_cast<size_t>(counts[1]) * sizeof(uint32_t);
    if (!counts[0] || vertexBytes > snapshot.size() - sizeof(counts) ||
        indexBytes != snapshot.size() - sizeof(counts) - vertexBytes)
        return false;
    std::vector<Vertex> vertices(counts[0]);
    std::vector<uint32_t> indices(counts[1]);
    std::memcpy(vertices.data(), snapshot.data() + sizeof(counts), vertexBytes);
    std::memcpy(indices.data(), snapshot.data() + sizeof(counts) + vertexBytes,
        indexBytes);
    return indices.empty()
        ? mesh.SetAuthoredVertices(std::move(vertices))
        : mesh.SetIndexedGeometry(std::move(vertices), std::move(indices));
}

bool StepMeshSnapshotHistory(Engine::Components::Mesh& mesh, bool redo,
    std::deque<std::string>& undo, std::deque<std::string>& redoEntries,
    std::string& baseline)
{
    return StepSnapshotHistory(redo, undo, redoEntries, baseline,
        [&](const std::string& target)
        { return RestoreMeshSnapshot(mesh, target); });
}
}
