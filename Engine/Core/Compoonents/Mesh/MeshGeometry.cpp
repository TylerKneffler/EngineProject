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
bool Mesh::SetDeformedVertices(const std::vector<Vertex>& vertices)
{
    const size_t byteSize = vertices.size() * sizeof(Vertex);
    if (byteSize == 0 ||
        (vertices.size() == m_vertices.size() &&
         std::memcmp(vertices.data(), m_vertices.data(), byteSize) == 0))
        return false;
    MakeGeometryWritable();
    ClearRuntimeLods();
    ClearSurfaceOffsets();

    // A real portal cut adds intersection vertices, so the two clipped halves
    // generally contain more vertices than the original mesh. Recreate the
    // upload buffer when its size changes instead of rejecting the cut.
    m_terrainVertices.clear();
    m_indices.clear();
    ReplaceIndexBuffer({});
    const bool canReuseBuffer = m_vertexBuffer &&
        m_vertexBuffer->GetAccessMode() == IGraphicsBuffer::AccessMode::Upload &&
        byteSize <= m_vertexBuffer->GetSize() &&
        !UsesDeferredGraphicsRetirement();
    if (!canReuseBuffer)
    {
        if (m_vertexBuffer || m_bufferFactory)
        {
            if (!m_bufferFactory)
                return false;
            auto replacement = m_bufferFactory->CreateBuffer(
                IGraphicsBuffer::Usage::VertexBuffer,
                IGraphicsBuffer::AccessMode::Upload, byteSize, vertices.data());
            if (!replacement)
                return false;
            ReplaceVertexBuffer(std::move(replacement));
        }
        m_vertices = vertices;
        UpdateBounds();
        m_ready = true;
        // Rigid bodies cache mesh-collider topology by component revision.
        // Portal cuts may add intersection vertices, so physics must rebuild
        // its collision/raycast representation before the next simulation step.
        MarkConfigurationDirty();
        return true;
    }

    m_vertices = vertices;
    UpdateBounds();
    if (m_vertexBuffer)
    {
        if (void* mapped = m_vertexBuffer->Map())
        {
            std::memcpy(mapped, m_vertices.data(), byteSize);
            m_vertexBuffer->Unmap();
        }
    }
    MarkConfigurationDirty();
    return true;
}

bool Mesh::SetDeformedVertices(std::vector<Vertex>&& vertices)
{
    const size_t byteSize = vertices.size() * sizeof(Vertex);
    if (byteSize == 0 ||
        (vertices.size() == m_vertices.size() &&
         std::memcmp(vertices.data(), m_vertices.data(), byteSize) == 0))
        return false;
    MakeGeometryWritable();
    ClearRuntimeLods();
    ClearSurfaceOffsets();

    m_terrainVertices.clear();
    m_indices.clear();
    ReplaceIndexBuffer({});
    const bool canReuseBuffer = m_vertexBuffer &&
        m_vertexBuffer->GetAccessMode() == IGraphicsBuffer::AccessMode::Upload &&
        byteSize <= m_vertexBuffer->GetSize() &&
        !UsesDeferredGraphicsRetirement();
    if (!canReuseBuffer && (m_vertexBuffer || m_bufferFactory))
    {
        if (!m_bufferFactory)
            return false;
        auto replacement = m_bufferFactory->CreateBuffer(
            IGraphicsBuffer::Usage::VertexBuffer,
            IGraphicsBuffer::AccessMode::Upload, byteSize, vertices.data());
        if (!replacement)
            return false;
        ReplaceVertexBuffer(std::move(replacement));
    }

    m_vertices = std::move(vertices);
    UpdateBounds();
    m_ready = true;
    if (m_vertexBuffer && canReuseBuffer)
    {
        if (void* mapped = m_vertexBuffer->Map())
        {
            std::memcpy(mapped, m_vertices.data(), byteSize);
            m_vertexBuffer->Unmap();
        }
    }
    MarkConfigurationDirty();
    return true;
}

bool Mesh::SetAuthoredVertices(std::vector<Vertex> vertices)
{
    if (!ValidateAuthoredGeometry(vertices, {}))
        return false;
    ClearRuntimeLods();
    ClearInstancing();
    ClearSurfaceOffsets();
    m_terrainVertices.clear();
    m_indices.clear();
    ReplaceIndexBuffer({});
    m_vertices = std::move(vertices);
    ++m_authoredGeometryRevision;
    MarkConfigurationDirty();
    UpdateBounds();
    if (m_bufferFactory)
        CreateBuffer(m_bufferFactory);
    return true;
}

bool Mesh::SetPreparedTriangleGeometry(std::vector<Vertex> vertices,
    const glm::vec3& minimum, const glm::vec3& maximum)
{
    if (vertices.empty() || vertices.size() % 3 != 0 || HasMorphTargets() ||
        !glm::all(glm::lessThanEqual(minimum, maximum))) return false;
    ClearRuntimeLods();
    ClearInstancing();
    ClearSurfaceOffsets();
    m_vertices = std::move(vertices);
    m_terrainVertices.clear();
    m_indices.clear();
    ReplaceIndexBuffer({});
    ReplaceVertexBuffer({});
    m_boundsMin = minimum;
    m_boundsMax = maximum;
    m_hasBounds = true;
    m_ready = false;
    ++m_authoredGeometryRevision;
    MarkConfigurationDirty();
    if (m_bufferFactory) CreateBuffer(m_bufferFactory);
    return true;
}

void Mesh::SetInstancedGeometry(std::shared_ptr<Mesh> source,
    std::shared_ptr<InstanceData> instances, std::vector<InstanceRange> ranges,
    const glm::vec3& minimum, const glm::vec3& maximum)
{
    static_assert(sizeof(InstanceTransform) == 48, "Instance transform layout must match Object.hlsl");
    if (!source || source.get() == this || source->IsInstanced() ||
        source->HasMorphTargets() || source->HasSurfaceOffsets() ||
        source->UsesTerrainVertexFormat() || !instances)
        throw std::invalid_argument("Instances require a static triangle-list source mesh");
    const uint32_t elementCount = source->GetIndexCount()
        ? source->GetIndexCount() : source->GetVertexCount();
    for (const auto& range : ranges)
        if (range.firstVertex % 3 || range.vertexCount % 3 ||
            uint64_t(range.firstVertex) + range.vertexCount > elementCount ||
            uint64_t(range.firstInstance) + range.instanceCount > instances->transforms.size())
            throw std::invalid_argument("Instance range exceeds shared geometry or placements");
    ClearRuntimeLods();
    m_vertices.clear(); m_indices.clear(); m_terrainVertices.clear();
    ReplaceVertexBuffer({}); ReplaceIndexBuffer({});
    m_instanceSource = std::move(source);
    m_instances = std::move(instances);
    m_instanceRanges = std::move(ranges);
    m_boundsMin = minimum; m_boundsMax = maximum; m_hasBounds = true;
    m_ready = m_instanceSource->IsReady();
    // Instance records are resident in the renderer's persistent pool on first
    // submission. Legacy per-range buffers are optional shader fallbacks.
    ++m_authoredGeometryRevision;
    MarkConfigurationDirty();
}

bool Mesh::ExchangeStaticGeometry(Mesh& other)
{
    if (HasMorphTargets() || other.HasMorphTargets() ||
        HasSurfaceOffsets() || other.HasSurfaceOffsets()) return false;
    using std::swap;
    swap(m_instanceSource, other.m_instanceSource);
    swap(m_instances, other.m_instances);
    swap(m_instanceRanges, other.m_instanceRanges);
    swap(m_vertices, other.m_vertices);
    swap(m_terrainVertices, other.m_terrainVertices);
    swap(m_indices, other.m_indices);
    swap(m_vertexBuffer, other.m_vertexBuffer);
    swap(m_sharedVertexBuffer, other.m_sharedVertexBuffer);
    swap(m_sharedIndexBuffer, other.m_sharedIndexBuffer);
    swap(m_indexBuffer, other.m_indexBuffer);
    swap(m_bufferFactory, other.m_bufferFactory);
    swap(m_ready, other.m_ready);
    swap(m_hasBounds, other.m_hasBounds);
    swap(m_boundsMin, other.m_boundsMin);
    swap(m_boundsMax, other.m_boundsMax);
    ++m_authoredGeometryRevision;
    ++other.m_authoredGeometryRevision;
    MarkConfigurationDirty();
    other.MarkConfigurationDirty();
    return true;
}

bool Mesh::UpdateAuthoredVertices(std::vector<Vertex> vertices)
{
    if (vertices.size() != m_vertices.size() || !m_terrainVertices.empty() ||
        !ValidateAuthoredGeometry(vertices, m_indices)) return false;
    MakeGeometryWritable();
    const size_t byteSize = vertices.size() * sizeof(Vertex);
    if (m_vertexBuffer)
    {
        if (byteSize > m_vertexBuffer->GetSize()) return false;
        if (m_vertexBuffer->GetAccessMode() != IGraphicsBuffer::AccessMode::Upload ||
            UsesDeferredGraphicsRetirement())
        {
            if (!m_bufferFactory) return false;
            auto replacement = m_bufferFactory->CreateBuffer(
                IGraphicsBuffer::Usage::VertexBuffer,
                IGraphicsBuffer::AccessMode::Upload, byteSize, vertices.data());
            if (!replacement) return false;
            ReplaceVertexBuffer(std::move(replacement));
        }
        else
        {
            void* mapped = m_vertexBuffer->Map();
            if (!mapped) return false;
            std::memcpy(mapped, vertices.data(), byteSize);
            m_vertexBuffer->Unmap();
        }
    }
    ClearRuntimeLods();
    ClearSurfaceOffsets();
    m_vertices = std::move(vertices);
    ++m_authoredGeometryRevision;
    MarkConfigurationDirty();
    UpdateBounds();
    if (!m_vertexBuffer && m_bufferFactory) CreateBuffer(m_bufferFactory);
    return true;
}

bool Mesh::UpdateIndexedVertexRange(size_t first,
    const std::vector<Vertex>& vertices)
{
    if (vertices.empty() || m_indices.empty() || !m_terrainVertices.empty() ||
        first > m_vertices.size() ||
        vertices.size() > m_vertices.size() - first) return false;
    MakeGeometryWritable();
    if (m_vertexBuffer &&
        m_vertexBuffer->GetAccessMode() == IGraphicsBuffer::AccessMode::Upload &&
        !UsesDeferredGraphicsRetirement())
    {
        void* mapped = m_vertexBuffer->Map();
        if (!mapped) return false;
        const size_t offset = first * sizeof(Vertex);
        const size_t bytes = vertices.size() * sizeof(Vertex);
        std::memcpy(static_cast<char*>(mapped) + offset, vertices.data(), bytes);
        m_vertexBuffer->Unmap();
        m_vertexBuffer->FlushMappedWrites(offset, bytes);
    }
    std::copy(vertices.begin(), vertices.end(), m_vertices.begin() + first);
    if (m_vertexBuffer &&
        (m_vertexBuffer->GetAccessMode() != IGraphicsBuffer::AccessMode::Upload ||
            UsesDeferredGraphicsRetirement()))
    {
        if (!m_bufferFactory) return false;
        auto replacement = m_bufferFactory->CreateBuffer(
            IGraphicsBuffer::Usage::VertexBuffer,
            IGraphicsBuffer::AccessMode::Upload,
            m_vertices.size() * sizeof(Vertex), m_vertices.data());
        if (!replacement) return false;
        ReplaceVertexBuffer(std::move(replacement));
    }
    ClearRuntimeLods();
    ClearSurfaceOffsets();
    ++m_authoredGeometryRevision;
    MarkConfigurationDirty();
    UpdateBounds();
    return true;
}

void Mesh::SetAuthoredFilePath(std::string path)
{
    if (m_filePath == path) return;
    m_filePath = std::move(path);
    MarkConfigurationDirty();
}

std::vector<Mesh::Vertex> Mesh::TakeVertices()
{
    ClearRuntimeLods();
    ClearSurfaceOffsets();
    m_ready = false;
    m_hasBounds = false;
    MarkConfigurationDirty();
    return std::move(m_vertices);
}

} // namespace Engine::Components

