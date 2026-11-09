#include "Mesh.h"
#include "Core/Rendering/SharedMeshBuffers.h"
#include "Core/Threading/JobScheduler.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Model/MeshPackAsset.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <fstream>
#include <algorithm>
#include <sstream>
#include <array>
#include <filesystem>
#include <stdexcept>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <chrono>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <glm/gtc/quaternion.hpp>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace Engine::Components
{
namespace
{
constexpr uint32_t kNativeMeshMagic = 0x4853454d;
constexpr uint32_t kNativeMeshVersion = 7;
}
void Mesh::UpdateBounds()
{
    m_hasBounds = !m_terrainVertices.empty() || !m_vertices.empty();
    if (!m_hasBounds)
    {
        m_boundsMin = {};
        m_boundsMax = {};
        return;
    }
    // Only indexed vertices can reach the rasterizer. Distant terrain tiles
    // retain a shared grid while their index buffer omits the moving near
    // window; including those unused vertices keeps offscreen tiles alive.
    const bool terrainFormat = !m_terrainVertices.empty();
    const size_t vertexCount = terrainFormat
        ? m_terrainVertices.size() : m_vertices.size();
    const bool indexed = !m_indices.empty();
    const uint32_t firstIndex = indexed && m_indices.front() < vertexCount
        ? m_indices.front() : 0u;
    const float* firstPosition = terrainFormat
        ? m_terrainVertices[firstIndex].pos : m_vertices[firstIndex].pos;
    m_boundsMin = { firstPosition[0], firstPosition[1], firstPosition[2] };
    m_boundsMax = m_boundsMin;
    const auto includePosition = [&](const float* positionValues)
    {
        const glm::vec3 position(positionValues[0], positionValues[1],
            positionValues[2]);
        m_boundsMin = glm::min(m_boundsMin, position);
        m_boundsMax = glm::max(m_boundsMax, position);
    };
    if (indexed)
    {
        for (uint32_t index : m_indices)
            if (index < vertexCount)
                includePosition(terrainFormat
                    ? m_terrainVertices[index].pos : m_vertices[index].pos);
    }
    else if (terrainFormat)
    {
        for (const TerrainVertex& vertex : m_terrainVertices)
            includePosition(vertex.pos);
    }
    else
    {
        for (const Vertex& vertex : m_vertices)
            includePosition(vertex.pos);
    }

    // GPU morphing no longer rebuilds bounds each frame. Expand the authored
    // bounds once by the full signed envelope of every target so any animated
    // position remains conservatively visible, including negative weights.
    if (!m_morphTargets.empty() && !m_vertices.empty())
    {
        for (size_t vertexIndex = 0; vertexIndex < m_vertices.size(); ++vertexIndex)
        {
            glm::vec3 extent(0.f);
            for (const MorphTarget& target : m_morphTargets)
                if (vertexIndex < target.positions.size())
                    extent += glm::abs(target.positions[vertexIndex]);
            const glm::vec3 base(m_vertices[vertexIndex].pos[0],
                m_vertices[vertexIndex].pos[1], m_vertices[vertexIndex].pos[2]);
            m_boundsMin = glm::min(m_boundsMin, base - extent);
            m_boundsMax = glm::max(m_boundsMax, base + extent);
        }
    }
}

bool Mesh::ValidateAuthoredGeometry(const std::vector<Vertex>& vertices,
    const std::vector<uint32_t>& indices)
{
    if (vertices.empty() || vertices.size() > UINT32_MAX ||
        indices.size() > UINT32_MAX ||
        (indices.empty() ? vertices.size() % 3u : indices.size() % 3u) != 0u)
        return false;
    for (const Vertex& vertex : vertices)
    {
        const auto finite = [](const float* values, size_t count)
        {
            for (size_t i = 0; i < count; ++i)
                if (!std::isfinite(values[i])) return false;
            return true;
        };
        if (!finite(vertex.pos, 3) || !finite(vertex.normal, 3) ||
            !finite(vertex.uv, 2) || !finite(vertex.tangent, 4) ||
            !finite(vertex.uv1, 2) || !finite(vertex.color, 4) ||
            !finite(vertex.joints0, 4) || !finite(vertex.weights0, 4) ||
            !finite(vertex.joints1, 4) || !finite(vertex.weights1, 4))
            return false;
        for (const auto& channel : vertex.uvChannels)
            if (!finite(channel, 4)) return false;
        if (!finite(vertex.sdefCenterAndMode, 4) ||
            !finite(vertex.sdefRadius0, 4) ||
            !finite(vertex.sdefRadius1, 4)) return false;
        for (float value : vertex.weights0)
            if (value < 0.f) return false;
        for (float value : vertex.weights1)
            if (value < 0.f) return false;
        for (float value : vertex.joints0)
            if (value < 0.f || std::floor(value) != value) return false;
        for (float value : vertex.joints1)
            if (value < 0.f || std::floor(value) != value) return false;
    }
    for (uint32_t index : indices)
        if (index >= vertices.size()) return false;
    return true;
}

bool Mesh::SaveNativeFile(const std::string& path,
    const std::vector<Vertex>& vertices)
{
    return SaveNativeFile(path, vertices, {});
}

bool Mesh::SaveNativeFile(const std::string& path,
    const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices)
{
    if (!ValidateAuthoredGeometry(vertices, indices)) return false;
    const std::filesystem::path target(path);
    const std::filesystem::path temporary = target.string() + ".tmp." +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    const uint32_t count = static_cast<uint32_t>(vertices.size());
    const uint32_t indexCount = static_cast<uint32_t>(indices.size());
    file.write(reinterpret_cast<const char*>(&kNativeMeshMagic), sizeof(kNativeMeshMagic));
    file.write(reinterpret_cast<const char*>(&kNativeMeshVersion), sizeof(kNativeMeshVersion));
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));
    file.write(reinterpret_cast<const char*>(&indexCount), sizeof(indexCount));
    file.write(reinterpret_cast<const char*>(vertices.data()),
        static_cast<std::streamsize>(vertices.size() * sizeof(Vertex)));
    file.write(reinterpret_cast<const char*>(indices.data()),
        static_cast<std::streamsize>(indices.size() * sizeof(uint32_t)));
    file.flush();
    const bool written = file.good();
    file.close();
    bool replaced = false;
    if (written)
    {
#ifdef _WIN32
        const std::wstring source = temporary.wstring();
        const std::wstring destination = target.wstring();
        replaced = std::filesystem::exists(target)
            ? ReplaceFileW(destination.c_str(), source.c_str(), nullptr,
                REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) != 0
            : MoveFileExW(source.c_str(), destination.c_str(),
                MOVEFILE_WRITE_THROUGH) != 0;
#else
        std::error_code renameError;
        std::filesystem::rename(temporary, target, renameError);
        replaced = !renameError;
#endif
    }
    if (!replaced)
    {
        std::error_code removeError;
        std::filesystem::remove(temporary, removeError);
    }
    return replaced;
}


} // namespace Engine::Components


