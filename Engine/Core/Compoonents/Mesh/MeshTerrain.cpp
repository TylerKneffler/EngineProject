#include "Mesh.h"
#include "Core/Compoonents/Terrain/TerrainSurfaceWinding.h"
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
struct TerrainVertexHash
{
    size_t operator()(const Engine::Model::TerrainVertex& vertex) const noexcept
    {
        const auto* bytes = reinterpret_cast<const uint8_t*>(&vertex);
        size_t hash = sizeof(size_t) == 8
            ? static_cast<size_t>(1469598103934665603ull)
            : static_cast<size_t>(2166136261u);
        const size_t prime = sizeof(size_t) == 8
            ? static_cast<size_t>(1099511628211ull)
            : static_cast<size_t>(16777619u);
        for (size_t i = 0; i < sizeof(vertex); ++i)
            hash = (hash ^ bytes[i]) * prime;
        return hash;
    }
};

struct TerrainVertexEqual
{
    bool operator()(const Engine::Model::TerrainVertex& first,
        const Engine::Model::TerrainVertex& second) const noexcept
    {
        return std::memcmp(&first, &second, sizeof(first)) == 0;
    }
};
}

Mesh::TerrainMeshData Mesh::BuildIndexedTerrain(
    const std::vector<Vertex>& vertices)
{
    TerrainMeshData result;
    result.vertices.reserve(vertices.size());
    result.indices.reserve(vertices.size());
    std::unordered_map<TerrainVertex, uint32_t,
        TerrainVertexHash, TerrainVertexEqual> unique;
    unique.reserve(vertices.size());
    for (const Vertex& source : vertices)
    {
        TerrainVertex packed{};
        std::memcpy(packed.pos, source.pos, sizeof(packed.pos));
        std::memcpy(packed.normal, source.normal, sizeof(packed.normal));
        std::memcpy(packed.uv, source.uv, sizeof(packed.uv));
        std::memcpy(packed.color, source.color, sizeof(packed.color));
        const auto [iterator, inserted] = unique.emplace(
            packed, static_cast<uint32_t>(result.vertices.size()));
        if (inserted)
            result.vertices.push_back(packed);
        result.indices.push_back(iterator->second);
    }
    result.vertices.shrink_to_fit();
    result.indices.shrink_to_fit();
    return result;
}

bool Mesh::SetTerrainGeometry(TerrainMeshData&& geometry,
    bool retainExpandedVertices)
{
    ClearRuntimeLods();
    ClearSurfaceOffsets();
    if (geometry.vertices.empty() || geometry.indices.empty())
        return false;
    ClearInstancing();

    OrientTerrainSurfaceUp(geometry.vertices, geometry.indices);
    m_terrainVertices = std::move(geometry.vertices);
    m_indices = std::move(geometry.indices);
    m_vertices.clear();
    if (retainExpandedVertices)
    {
        m_vertices.reserve(m_indices.size());
        for (uint32_t index : m_indices)
        {
            if (index >= m_terrainVertices.size())
                continue;
            const TerrainVertex& source = m_terrainVertices[index];
            Vertex expanded{};
            std::memcpy(expanded.pos, source.pos, sizeof(source.pos));
            std::memcpy(expanded.normal, source.normal, sizeof(source.normal));
            std::memcpy(expanded.uv, source.uv, sizeof(source.uv));
            std::memcpy(expanded.color, source.color, sizeof(source.color));
            m_vertices.push_back(expanded);
        }
    }

    UpdateBounds();
    const uint64_t vertexBytes = static_cast<uint64_t>(m_terrainVertices.size()) *
        sizeof(TerrainVertex);
    const uint64_t indexBytes = static_cast<uint64_t>(m_indices.size()) *
        sizeof(uint32_t);
    if (m_bufferFactory)
    {
        MakeGeometryWritable();
        const auto upload = [&](std::shared_ptr<IGraphicsBuffer>& buffer,
            IGraphicsBuffer::Usage usage, uint64_t byteCount, const void* data)
        {
            if (!buffer || buffer->GetSize() < byteCount ||
                UsesDeferredGraphicsRetirement())
            {
                auto replacement = m_bufferFactory->CreateBuffer(usage,
                    IGraphicsBuffer::AccessMode::Upload, byteCount, data);
                if (&buffer == &m_vertexBuffer)
                    ReplaceVertexBuffer(std::move(replacement));
                else
                    ReplaceIndexBuffer(std::move(replacement));
            }
            else if (void* mapped = buffer->Map())
            {
                std::memcpy(mapped, data, static_cast<size_t>(byteCount));
                buffer->Unmap();
            }
            return buffer != nullptr;
        };
        if (!upload(m_vertexBuffer, IGraphicsBuffer::Usage::VertexBuffer,
                vertexBytes, m_terrainVertices.data()) ||
            !upload(m_indexBuffer, IGraphicsBuffer::Usage::IndexBuffer,
                indexBytes, m_indices.data()))
            return false;
    }
    m_ready = m_bufferFactory == nullptr || (m_vertexBuffer && m_indexBuffer);
    MarkConfigurationDirty();
    return true;
}

Mesh::TerrainMeshData Mesh::TakeTerrainGeometry()
{
    ClearRuntimeLods();
    ClearSurfaceOffsets();
    TerrainMeshData result{ std::move(m_terrainVertices), std::move(m_indices) };
    m_vertices.clear();
    m_ready = false;
    m_hasBounds = false;
    MarkConfigurationDirty();
    return result;
}

uint64_t Mesh::GetCpuMeshMemoryBytes() const
{
    uint64_t bytes = static_cast<uint64_t>(m_vertices.capacity()) * sizeof(Vertex) +
        static_cast<uint64_t>(m_terrainVertices.capacity()) * sizeof(TerrainVertex) +
        static_cast<uint64_t>(m_indices.capacity()) * sizeof(uint32_t);
    for (const MorphTarget& target : m_morphTargets)
    {
        bytes += static_cast<uint64_t>(target.positions.capacity() +
            target.normals.capacity() + target.tangents.capacity()) *
            sizeof(glm::vec3) + static_cast<uint64_t>(target.uv.capacity()) *
            sizeof(glm::vec4);
        for (const auto& channel : target.uvChannels)
            bytes += static_cast<uint64_t>(channel.capacity()) * sizeof(glm::vec4);
    }
    return bytes + static_cast<uint64_t>(m_morphWeights.capacity()) * sizeof(float) +
        static_cast<uint64_t>(m_surfaceOffsets.capacity()) * sizeof(glm::vec3);
}

uint64_t Mesh::GetUploadShadowMemoryBytes() const
{
    return (m_vertexBuffer ? m_vertexBuffer->GetUploadShadowSize() : 0u) +
        (m_posedVertexBuffer ? m_posedVertexBuffer->GetUploadShadowSize() : 0u) +
        (m_indexBuffer ? m_indexBuffer->GetUploadShadowSize() : 0u) +
        (m_morphDeltaBuffer ? m_morphDeltaBuffer->GetUploadShadowSize() : 0u) +
        (m_morphWeightBuffer ? m_morphWeightBuffer->GetUploadShadowSize() : 0u);
}

uint64_t Mesh::GetGpuBufferMemoryBytes() const
{
    if (IsInstanced())
    {
        uint64_t bytes = m_instanceSource->GetGpuBufferMemoryBytes();
        for (const auto& slice : m_instances->slices)
            if (slice.buffer) bytes += slice.buffer->GetSize();
        return bytes;
    }
    return (m_vertexBuffer ? m_vertexBuffer->GetSize() : 0u) +
        (m_posedVertexBuffer ? m_posedVertexBuffer->GetSize() : 0u) +
        (m_indexBuffer ? m_indexBuffer->GetSize() : 0u) +
        (m_morphDeltaBuffer ? m_morphDeltaBuffer->GetSize() : 0u) +
        (m_morphWeightBuffer ? m_morphWeightBuffer->GetSize() : 0u);
}

void Mesh::InitializeRuntimeCloneFrom(const Mesh& source)
{
    m_filePath = source.m_filePath;
    m_bufferFactory = source.m_bufferFactory;
}

} // namespace Engine::Components


