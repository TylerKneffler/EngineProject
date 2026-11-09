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

bool Mesh::SetIndexedGeometry(std::vector<Vertex> vertices,
    std::vector<uint32_t> indices)
{
    const auto started = std::chrono::steady_clock::now();
    m_lastIndexedGeometryTiming = {};
    if (indices.empty() || !ValidateAuthoredGeometry(vertices, indices))
        return false;
    const auto validated = std::chrono::steady_clock::now();
    ClearRuntimeLods();
    ClearInstancing();
    ClearSurfaceOffsets();
    m_vertices = std::move(vertices);
    m_indices = std::move(indices);
    m_terrainVertices.clear();
    ++m_authoredGeometryRevision;
    MarkConfigurationDirty();
    UpdateBounds();
    const auto bounded = std::chrono::steady_clock::now();
    if (m_bufferFactory) CreateBuffer(m_bufferFactory);
    const auto buffered = std::chrono::steady_clock::now();
    m_lastIndexedGeometryTiming.validationMs =
        std::chrono::duration<double, std::milli>(validated - started).count();
    m_lastIndexedGeometryTiming.boundsMs =
        std::chrono::duration<double, std::milli>(bounded - validated).count();
    m_lastIndexedGeometryTiming.bufferMs =
        std::chrono::duration<double, std::milli>(buffered - bounded).count();
    return true;
}

std::optional<Mesh::IndexedGeometryBounds> Mesh::PrepareIndexedGeometry(
    const std::vector<Vertex>& vertices,
    const std::vector<uint32_t>& indices)
{
    if (indices.empty() || !ValidateAuthoredGeometry(vertices, indices))
        return std::nullopt;
    const float* first = vertices[indices.front()].pos;
    IndexedGeometryBounds bounds{
        {first[0], first[1], first[2]},
        {first[0], first[1], first[2]}};
    for (uint32_t index : indices)
    {
        const auto& vertex = vertices[index];
        const glm::vec3 position(vertex.pos[0], vertex.pos[1], vertex.pos[2]);
        bounds.minimum = glm::min(bounds.minimum, position);
        bounds.maximum = glm::max(bounds.maximum, position);
    }
    return bounds;
}

bool Mesh::SetPreparedIndexedGeometry(std::vector<Vertex> vertices,
    std::vector<uint32_t> indices, IndexedGeometryBounds bounds)
{
    if (vertices.empty() || indices.empty() || !m_morphTargets.empty())
        return false;
    const auto started = std::chrono::steady_clock::now();
    m_lastIndexedGeometryTiming = {};
    ClearRuntimeLods();
    ClearInstancing();
    ClearSurfaceOffsets();
    m_vertices = std::move(vertices);
    m_indices = std::move(indices);
    m_terrainVertices.clear();
    ++m_authoredGeometryRevision;
    MarkConfigurationDirty();
    m_boundsMin = bounds.minimum;
    m_boundsMax = bounds.maximum;
    m_hasBounds = true;
    if (m_bufferFactory) CreateBuffer(m_bufferFactory);
    m_lastIndexedGeometryTiming.bufferMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
    return true;
}

#pragma region DX12 buffer creation and rendering

void Mesh::MakeGeometryWritable()
{
    if (!m_bufferFactory) return;
    if (m_sharedVertexBuffer && m_vertexBuffer)
    {
        const void* data = UsesTerrainVertexFormat() ? static_cast<const void*>(m_terrainVertices.data()) : m_vertices.data();
        ReplaceVertexBuffer(m_bufferFactory->CreateBuffer(IGraphicsBuffer::Usage::VertexBuffer,
            IGraphicsBuffer::AccessMode::Upload, GetVertexCount() * uint64_t(GetVertexStride()), data));
    }
    if (m_sharedIndexBuffer && m_indexBuffer)
        ReplaceIndexBuffer(m_bufferFactory->CreateBuffer(IGraphicsBuffer::Usage::IndexBuffer,
            IGraphicsBuffer::AccessMode::Upload, m_indices.size() * sizeof(uint32_t), m_indices.data()));
    m_sharedVertexBuffer = m_sharedIndexBuffer = false;
}

void Mesh::CreateBuffer(IGraphicsBufferFactory* bufferFactory)
{
    if (!bufferFactory || (m_vertices.empty() && m_terrainVertices.empty()))
        return;

    m_bufferFactory = bufferFactory;
    if (!m_terrainVertices.empty())
    {
        const uint64_t vertexBytes = static_cast<uint64_t>(
            m_terrainVertices.size()) * sizeof(TerrainVertex);
        const uint64_t indexBytes = static_cast<uint64_t>(m_indices.size()) *
            sizeof(uint32_t);
        ReplaceVertexBuffer(Engine::Rendering::AcquireMeshBuffer(bufferFactory,
            IGraphicsBuffer::Usage::VertexBuffer, m_terrainVertices.data(), vertexBytes, sizeof(TerrainVertex)));
        m_sharedVertexBuffer = true;
        ReplaceIndexBuffer(Engine::Rendering::AcquireMeshBuffer(bufferFactory,
            IGraphicsBuffer::Usage::IndexBuffer, m_indices.data(), indexBytes, sizeof(uint32_t)));
        m_sharedIndexBuffer = true;
        if (!m_vertexBuffer || !m_indexBuffer)
            throw std::runtime_error("Failed to create indexed terrain buffers");
        m_ready = true;
        return;
    }
    const uint64_t byteSize = m_vertices.size() * sizeof(Vertex);

    ReplaceVertexBuffer(Engine::Rendering::AcquireMeshBuffer(bufferFactory,
        IGraphicsBuffer::Usage::VertexBuffer, m_vertices.data(), byteSize, sizeof(Vertex)));
    m_sharedVertexBuffer = true;

    if (!m_vertexBuffer)
        throw std::runtime_error("Failed to create vertex buffer");

    CreateMorphBuffers();
    if (!m_indices.empty())
    {
        const uint64_t indexBytes = static_cast<uint64_t>(m_indices.size()) *
            sizeof(uint32_t);
        ReplaceIndexBuffer(Engine::Rendering::AcquireMeshBuffer(bufferFactory,
            IGraphicsBuffer::Usage::IndexBuffer, m_indices.data(), indexBytes, sizeof(uint32_t)));
        m_sharedIndexBuffer = true;
        if (!m_indexBuffer)
            throw std::runtime_error("Failed to create mesh index buffer");
    }
    else ReplaceIndexBuffer({});
    m_ready = true;
}
#pragma endregion

} // namespace Engine::Components

