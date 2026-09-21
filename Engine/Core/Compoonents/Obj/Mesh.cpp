#include "Mesh.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <fstream>
#include <sstream>
#include <array>
#include <filesystem>
#include <stdexcept>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <unordered_map>

namespace Engine::Components
{
Mesh::Mesh()
{
    SetTypeName(COMPONENT_TYPE_NAME(Mesh));
    RegisterField("file", m_filePath);
}

#ifndef ENGINE_ASSETS_PATH
#define ENGINE_ASSETS_PATH "Engine/Core/Assets/"
#endif

std::string Mesh::ResolveFilePath(const std::string& path)
{
    const std::filesystem::path requested(path);
    if (std::filesystem::exists(requested))
        return requested.lexically_normal().generic_string();

    // Scenes distributed with a project deliberately store portable paths such
    // as Assets/Mesh/cube.obj. In the project-free Engine Sandbox, resolve that
    // same path against the built-in template assets instead.
    const std::filesystem::path projectAssets("Assets");
    const std::filesystem::path relativeAsset = requested.lexically_relative(projectAssets);
    if (!relativeAsset.empty() && *relativeAsset.begin() != "..")
    {
        const std::filesystem::path engineAsset =
            std::filesystem::path(ENGINE_ASSETS_PATH) / relativeAsset;
        if (std::filesystem::exists(engineAsset))
            return engineAsset.lexically_normal().generic_string();
    }

    return requested.lexically_normal().generic_string();
}

namespace
{
constexpr uint32_t kNativeMeshMagic = 0x4853454d; // "MESH"
constexpr uint32_t kNativeMeshVersion = 4;
constexpr float kPi = 3.14159265358979323846f;
struct alignas(16) GpuMorphDelta
{
    glm::vec4 position{};
    glm::vec4 normal{};
    glm::vec4 tangent{};
};
static_assert(sizeof(GpuMorphDelta) == 48,
    "GPU morph delta layout must match Object.hlsl");
struct LegacyVertexV2
{
    float pos[3], normal[3], uv[2], tangent[4];
};
struct LegacyVertexV3
{
    float pos[3], normal[3], uv[2], tangent[4], uv1[2], color[4];
};

Engine::Serialization::JsonValue FloatArray(const std::vector<float>& values)
{
    Engine::Serialization::JsonValue result = Engine::Serialization::JsonValue::MakeArray();
    for (float value : values) result.Push(Engine::Serialization::JsonValue(value));
    return result;
}

Engine::Serialization::JsonValue Vec3Array(const std::vector<glm::vec3>& values)
{
    Engine::Serialization::JsonValue result = Engine::Serialization::JsonValue::MakeArray();
    for (const glm::vec3& value : values)
        result.Push(Engine::Serialization::JsonValue::MakeArray().Push(Engine::Serialization::JsonValue(value.x))
            .Push(Engine::Serialization::JsonValue(value.y)).Push(Engine::Serialization::JsonValue(value.z)));
    return result;
}

std::vector<glm::vec3> ReadVec3Array(const Engine::Serialization::JsonValue& value)
{
    std::vector<glm::vec3> result;
    for (size_t i = 0; i < value.ArraySize(); ++i)
    {
        const Engine::Serialization::JsonValue& item = value.ArrayAt(i);
        result.emplace_back(item.ArrayAt(0).AsFloat(), item.ArrayAt(1).AsFloat(),
            item.ArrayAt(2).AsFloat());
    }
    return result;
}

Mesh::Vertex SphereVertex(float longitude, float latitude, float u, float v)
{
    const float sinLatitude = std::sin(latitude);
    const glm::vec3 normal(
        sinLatitude * std::cos(longitude),
        std::cos(latitude),
        sinLatitude * std::sin(longitude));
    const glm::vec3 tangent(-std::sin(longitude), 0.f, std::cos(longitude));
    Mesh::Vertex vertex{};
    vertex.pos[0] = normal.x * 0.5f;
    vertex.pos[1] = normal.y * 0.5f;
    vertex.pos[2] = normal.z * 0.5f;
    vertex.normal[0] = normal.x;
    vertex.normal[1] = normal.y;
    vertex.normal[2] = normal.z;
    vertex.uv[0] = u;
    vertex.uv[1] = v;
    vertex.tangent[0] = tangent.x;
    vertex.tangent[1] = tangent.y;
    vertex.tangent[2] = tangent.z;
    vertex.tangent[3] = 1.f;
    return vertex;
}

std::vector<Mesh::Vertex> GenerateSmoothSphere()
{
    constexpr uint32_t longitudeSegments = 48;
    constexpr uint32_t latitudeSegments = 24;
    std::vector<Mesh::Vertex> vertices;
    vertices.reserve(longitudeSegments * (latitudeSegments - 1) * 6);

    auto emitTriangle = [&](Mesh::Vertex first, Mesh::Vertex second,
                            Mesh::Vertex third)
    {
        const glm::vec3 a(first.pos[0], first.pos[1], first.pos[2]);
        const glm::vec3 b(second.pos[0], second.pos[1], second.pos[2]);
        const glm::vec3 c(third.pos[0], third.pos[1], third.pos[2]);
        if (glm::dot(glm::cross(b - a, c - a), a + b + c) < 0.f)
            std::swap(second, third);
        vertices.push_back(first);
        vertices.push_back(second);
        vertices.push_back(third);
    };

    for (uint32_t latitudeIndex = 0;
         latitudeIndex < latitudeSegments; ++latitudeIndex)
    {
        const float v0 = static_cast<float>(latitudeIndex) / latitudeSegments;
        const float v1 = static_cast<float>(latitudeIndex + 1) / latitudeSegments;
        const float latitude0 = v0 * kPi;
        const float latitude1 = v1 * kPi;
        for (uint32_t longitudeIndex = 0;
             longitudeIndex < longitudeSegments; ++longitudeIndex)
        {
            const float u0 = static_cast<float>(longitudeIndex) / longitudeSegments;
            const float u1 = static_cast<float>(longitudeIndex + 1) / longitudeSegments;
            const float longitude0 = u0 * 2.f * kPi;
            const float longitude1 = u1 * 2.f * kPi;
            const auto topLeft = SphereVertex(longitude0, latitude0, u0, v0);
            const auto topRight = SphereVertex(longitude1, latitude0, u1, v0);
            const auto bottomLeft = SphereVertex(longitude0, latitude1, u0, v1);
            const auto bottomRight = SphereVertex(longitude1, latitude1, u1, v1);
            if (latitudeIndex != 0)
                emitTriangle(topLeft, bottomLeft, topRight);
            if (latitudeIndex + 1 != latitudeSegments)
                emitTriangle(topRight, bottomLeft, bottomRight);
        }
    }
    return vertices;
}
}

#pragma region OBJ file parsing helpers
static void ParseFaceToken(const std::string& t, int& vi, int& vti, int& vni)
{
    vi = vti = vni = 0;
    size_t a = t.find('/');
    if (a == std::string::npos) { vi = std::stoi(t); return; }
    vi = std::stoi(t.substr(0, a));
    size_t b = t.find('/', a + 1);
    if (b == std::string::npos)
    {
        if (a + 1 < t.size()) vti = std::stoi(t.substr(a + 1));
        return;
    }
    if (b > a + 1) vti = std::stoi(t.substr(a + 1, b - a - 1));
    if (b + 1 < t.size()) vni = std::stoi(t.substr(b + 1));
}
#pragma endregion
#pragma region Mesh implementation

void Mesh::LoadFromFile(const std::string& path)
{
    MarkConfigurationDirty();
    m_terrainVertices.clear();
    m_indices.clear();
    m_indexBuffer.reset();
    m_filePath = path;  // store for serialization
    const std::filesystem::path resolvedPath = ResolveFilePath(path);
    if (resolvedPath.filename() == "sphere.obj")
    {
        // The built-in sphere is procedural so reflective silhouettes have
        // enough geometric resolution without carrying a large OBJ asset.
        m_vertices = GenerateSmoothSphere();
        UpdateBounds();
        m_ready = false;
        return;
    }
    if (resolvedPath.extension() == ".mesh")
    {
        std::ifstream native(resolvedPath, std::ios::binary);
        uint32_t magic = 0, version = 0, count = 0;
        native.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        native.read(reinterpret_cast<char*>(&version), sizeof(version));
        native.read(reinterpret_cast<char*>(&count), sizeof(count));
        if (!native || magic != kNativeMeshMagic ||
            (version != 2 && version != 3 && version != kNativeMeshVersion))
            throw std::runtime_error("Mesh: invalid native mesh: " + path);
        m_vertices.assign(count, Vertex{});
        if (version == 2)
        {
            std::vector<LegacyVertexV2> legacy(count);
            native.read(reinterpret_cast<char*>(legacy.data()),
                static_cast<std::streamsize>(legacy.size() * sizeof(LegacyVertexV2)));
            for (size_t i = 0; i < legacy.size(); ++i)
            {
                std::copy(std::begin(legacy[i].pos), std::end(legacy[i].pos), m_vertices[i].pos);
                std::copy(std::begin(legacy[i].normal), std::end(legacy[i].normal), m_vertices[i].normal);
                std::copy(std::begin(legacy[i].uv), std::end(legacy[i].uv), m_vertices[i].uv);
                std::copy(std::begin(legacy[i].tangent), std::end(legacy[i].tangent), m_vertices[i].tangent);
            }
        }
        else if (version == 3)
        {
            std::vector<LegacyVertexV3> legacy(count);
            native.read(reinterpret_cast<char*>(legacy.data()),
                static_cast<std::streamsize>(legacy.size() * sizeof(LegacyVertexV3)));
            for (size_t i = 0; i < legacy.size(); ++i)
            {
                std::memcpy(m_vertices[i].pos, legacy[i].pos, sizeof(LegacyVertexV3));
            }
        }
        else
            native.read(reinterpret_cast<char*>(m_vertices.data()),
                static_cast<std::streamsize>(m_vertices.size() * sizeof(Vertex)));
        if (!native)
            throw std::runtime_error("Mesh: truncated native mesh: " + path);
        UpdateBounds();
        m_ready = false;
        return;
    }

    std::ifstream file(resolvedPath);
    if (!file.is_open())
        throw std::runtime_error("Mesh: failed to open OBJ: " + path +
            " (resolved to " + resolvedPath.string() + ")");

    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 4>> colors;
    std::vector<std::array<float, 3>> normals;
    std::vector<std::array<float, 2>> texcoords;
    m_vertices.clear();

    std::string line;
    while (std::getline(file, line))
    {
        std::istringstream ss(line);
        std::string token;
        ss >> token;

        if (token == "v")
        {
            std::array<float, 3> p{};
            ss >> p[0] >> p[1] >> p[2];
            positions.push_back(p);
            // Common OBJ extension: optional RGB/RGBA values follow XYZ.
            // Keeping these indexed with positions lets diagnostic meshes
            // supply face colors without requiring a texture atlas.
            std::array<float, 4> color { 1.f, 1.f, 1.f, 1.f };
            if (ss >> color[0] >> color[1] >> color[2])
                ss >> color[3];
            colors.push_back(color);
        }
        else if (token == "vn")
        {
            std::array<float, 3> n{};
            ss >> n[0] >> n[1] >> n[2];
            normals.push_back(n);
        }
        else if (token == "vt")
        {
            std::array<float, 2> uv{};
            ss >> uv[0] >> uv[1];
            texcoords.push_back(uv);
        }
        else if (token == "f")
        {
            std::string t0, t1, t2;
            ss >> t0 >> t1 >> t2;
            for (auto& tok : { t0, t1, t2 })
            {
                int vi = 0, vti = 0, vni = 0;
                ParseFaceToken(tok, vi, vti, vni);
                Vertex v{};
                if (vi  > 0) { auto& p = positions[vi  - 1]; v.pos[0]    = p[0]; v.pos[1]    = p[1]; v.pos[2]    = p[2];
                    const auto& color = colors[vi - 1];
                    std::copy(color.begin(), color.end(), v.color); }
                if (vti > 0) { auto& uv = texcoords[vti - 1]; v.uv[0] = uv[0]; v.uv[1] = uv[1]; }
                if (vni > 0) { auto& n = normals  [vni - 1]; v.normal[0] = n[0]; v.normal[1] = n[1]; v.normal[2] = n[2]; }
                m_vertices.push_back(v);
            }
        }
    }

    UpdateBounds();
    m_ready = false;
}

bool Mesh::SetDeformedVertices(const std::vector<Vertex>& vertices)
{
    const size_t byteSize = vertices.size() * sizeof(Vertex);
    if (byteSize == 0 ||
        (vertices.size() == m_vertices.size() &&
         std::memcmp(vertices.data(), m_vertices.data(), byteSize) == 0))
        return false;

    // A real portal cut adds intersection vertices, so the two clipped halves
    // generally contain more vertices than the original mesh. Recreate the
    // upload buffer when its size changes instead of rejecting the cut.
    m_terrainVertices.clear();
    m_indices.clear();
    m_indexBuffer.reset();
    const bool sizeChanged = vertices.size() != m_vertices.size();
    const bool canReuseBuffer = m_vertexBuffer &&
        byteSize <= m_vertexBuffer->GetSize();
    if (sizeChanged && !canReuseBuffer)
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
            m_vertexBuffer = std::move(replacement);
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

    m_terrainVertices.clear();
    m_indices.clear();
    m_indexBuffer.reset();
    const bool sizeChanged = vertices.size() != m_vertices.size();
    const bool canReuseBuffer = m_vertexBuffer &&
        byteSize <= m_vertexBuffer->GetSize();
    if (sizeChanged && !canReuseBuffer && (m_vertexBuffer || m_bufferFactory))
    {
        if (!m_bufferFactory)
            return false;
        auto replacement = m_bufferFactory->CreateBuffer(
            IGraphicsBuffer::Usage::VertexBuffer,
            IGraphicsBuffer::AccessMode::Upload, byteSize, vertices.data());
        if (!replacement)
            return false;
        m_vertexBuffer = std::move(replacement);
    }

    m_vertices = std::move(vertices);
    UpdateBounds();
    m_ready = true;
    if (m_vertexBuffer && (!sizeChanged || canReuseBuffer))
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

std::vector<Mesh::Vertex> Mesh::TakeVertices()
{
    m_ready = false;
    m_hasBounds = false;
    MarkConfigurationDirty();
    return std::move(m_vertices);
}

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
    if (geometry.vertices.empty() || geometry.indices.empty())
        return false;

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
        const auto upload = [&](std::unique_ptr<IGraphicsBuffer>& buffer,
            IGraphicsBuffer::Usage usage, uint64_t byteCount, const void* data)
        {
            if (!buffer || buffer->GetSize() < byteCount)
                buffer = m_bufferFactory->CreateBuffer(usage,
                    IGraphicsBuffer::AccessMode::Upload, byteCount, data);
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
        bytes += static_cast<uint64_t>(target.positions.capacity() +
            target.normals.capacity() + target.tangents.capacity()) *
            sizeof(glm::vec3);
    return bytes + static_cast<uint64_t>(m_morphWeights.capacity()) * sizeof(float);
}

uint64_t Mesh::GetUploadShadowMemoryBytes() const
{
    return (m_vertexBuffer ? m_vertexBuffer->GetUploadShadowSize() : 0u) +
        (m_indexBuffer ? m_indexBuffer->GetUploadShadowSize() : 0u) +
        (m_morphDeltaBuffer ? m_morphDeltaBuffer->GetUploadShadowSize() : 0u) +
        (m_morphWeightBuffer ? m_morphWeightBuffer->GetUploadShadowSize() : 0u);
}

uint64_t Mesh::GetGpuBufferMemoryBytes() const
{
    return (m_vertexBuffer ? m_vertexBuffer->GetSize() : 0u) +
        (m_indexBuffer ? m_indexBuffer->GetSize() : 0u) +
        (m_morphDeltaBuffer ? m_morphDeltaBuffer->GetSize() : 0u) +
        (m_morphWeightBuffer ? m_morphWeightBuffer->GetSize() : 0u);
}

void Mesh::InitializeRuntimeCloneFrom(const Mesh& source)
{
    m_filePath = source.m_filePath;
    m_bufferFactory = source.m_bufferFactory;
}

void Mesh::SetMorphData(unsigned nodeIndex, std::vector<MorphTarget> targets,
    std::vector<float> weights)
{
    m_morphNodeIndex = nodeIndex;
    m_morphTargets = std::move(targets);
    m_morphWeights = std::move(weights);
    m_morphWeights.resize(m_morphTargets.size(), 0.f);
    m_observedMorphWeights = m_morphWeights;
    m_morphWeightsObserved = true;
    AdvanceMorphWeightsRevision();
    UpdateBounds();
    CreateMorphBuffers();
    MarkConfigurationDirty();
}

namespace
{
bool SameMorphWeights(const std::vector<float>& first,
    const std::vector<float>& second)
{
    return first.size() == second.size() &&
        (first.empty() || std::memcmp(first.data(), second.data(),
            first.size() * sizeof(float)) == 0);
}
}

void Mesh::AdvanceMorphWeightsRevision() const
{
    if (++m_morphWeightsRevision == 0)
        ++m_morphWeightsRevision;
}

void Mesh::ObserveMorphWeights() const
{
    if (m_morphWeightsObserved &&
        SameMorphWeights(m_morphWeights, m_observedMorphWeights))
        return;
    if (m_morphWeightsObserved)
        AdvanceMorphWeightsRevision();
    m_observedMorphWeights = m_morphWeights;
    m_morphWeightsObserved = true;
}

bool Mesh::SetMorphWeights(const std::vector<float>& weights)
{
    ObserveMorphWeights();
    if (SameMorphWeights(m_morphWeights, weights))
        return false;
    m_morphWeights = weights;
    m_observedMorphWeights = m_morphWeights;
    m_morphWeightsObserved = true;
    AdvanceMorphWeightsRevision();
    UploadMorphWeights();
    MarkConfigurationDirty();
    return true;
}

void Mesh::SyncMorphWeights()
{
    const uint64_t revision = m_morphWeightsRevision;
    ObserveMorphWeights();
    if (revision != m_morphWeightsRevision)
    {
        UploadMorphWeights();
        MarkConfigurationDirty();
    }
}

uint64_t Mesh::GetMorphWeightsRevision() const
{
    ObserveMorphWeights();
    return m_morphWeightsRevision;
}

void Mesh::CreateMorphBuffers()
{
    m_morphDeltaBuffer.reset();
    m_morphWeightBuffer.reset();
    m_packedMorphWeights.clear();
    if (!m_bufferFactory || m_vertices.empty() || m_morphTargets.empty())
        return;

    const size_t vertexCount = m_vertices.size();
    std::vector<GpuMorphDelta> deltas;
    deltas.resize(vertexCount * m_morphTargets.size());
    for (size_t targetIndex = 0; targetIndex < m_morphTargets.size(); ++targetIndex)
    {
        const MorphTarget& target = m_morphTargets[targetIndex];
        GpuMorphDelta* destination = deltas.data() + targetIndex * vertexCount;
        for (size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
        {
            if (vertexIndex < target.positions.size())
                destination[vertexIndex].position = glm::vec4(
                    target.positions[vertexIndex], 0.f);
            if (vertexIndex < target.normals.size())
                destination[vertexIndex].normal = glm::vec4(
                    target.normals[vertexIndex], 0.f);
            if (vertexIndex < target.tangents.size())
                destination[vertexIndex].tangent = glm::vec4(
                    target.tangents[vertexIndex], 0.f);
        }
    }
    m_morphDeltaBuffer = m_bufferFactory->CreateBuffer(
        IGraphicsBuffer::Usage::ShaderResource,
        IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(deltas.size()) * sizeof(GpuMorphDelta),
        deltas.data(), sizeof(GpuMorphDelta));

    const size_t weightGroups = (m_morphTargets.size() + 3u) / 4u;
    m_packedMorphWeights.assign(weightGroups, glm::vec4(0.f));
    m_morphWeightBuffer = m_bufferFactory->CreateBuffer(
        IGraphicsBuffer::Usage::ShaderResource,
        IGraphicsBuffer::AccessMode::Upload,
        static_cast<uint64_t>(m_packedMorphWeights.size()) * sizeof(glm::vec4),
        m_packedMorphWeights.data(), sizeof(glm::vec4));
    UploadMorphWeights();
}

void Mesh::UploadMorphWeights()
{
    if (!m_morphWeightBuffer)
        return;
    const size_t groupCount = (m_morphTargets.size() + 3u) / 4u;
    m_packedMorphWeights.assign(groupCount, glm::vec4(0.f));
    for (size_t index = 0; index < m_morphWeights.size() &&
        index < m_morphTargets.size(); ++index)
        m_packedMorphWeights[index / 4u][static_cast<glm::length_t>(index % 4u)] =
            m_morphWeights[index];
    if (void* mapped = m_morphWeightBuffer->Map())
    {
        const uint64_t byteCount = static_cast<uint64_t>(m_packedMorphWeights.size()) *
            sizeof(glm::vec4);
        std::memcpy(mapped, m_packedMorphWeights.data(), static_cast<size_t>(byteCount));
        m_morphWeightBuffer->Unmap();
        m_morphWeightBuffer->FlushMappedWrites(0, byteCount);
    }
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
    const float* firstPosition = !m_terrainVertices.empty()
        ? m_terrainVertices.front().pos : m_vertices.front().pos;
    m_boundsMin = { firstPosition[0], firstPosition[1], firstPosition[2] };
    m_boundsMax = m_boundsMin;
    const auto includePosition = [&](const float* positionValues)
    {
        const glm::vec3 position(positionValues[0], positionValues[1],
            positionValues[2]);
        m_boundsMin = glm::min(m_boundsMin, position);
        m_boundsMax = glm::max(m_boundsMax, position);
    };
    for (const Vertex& vertex : m_vertices)
        includePosition(vertex.pos);
    for (const TerrainVertex& vertex : m_terrainVertices)
        includePosition(vertex.pos);

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

bool Mesh::SaveNativeFile(const std::string& path, const std::vector<Vertex>& vertices)
{
    std::ofstream file(path, std::ios::binary);
    if (!file)
        return false;
    const uint32_t count = static_cast<uint32_t>(vertices.size());
    file.write(reinterpret_cast<const char*>(&kNativeMeshMagic), sizeof(kNativeMeshMagic));
    file.write(reinterpret_cast<const char*>(&kNativeMeshVersion), sizeof(kNativeMeshVersion));
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));
    file.write(reinterpret_cast<const char*>(vertices.data()),
        static_cast<std::streamsize>(vertices.size() * sizeof(Vertex)));
    return file.good();
}

namespace
{
float SignedDistanceToPlane(const glm::vec3& point,
    const glm::vec3& planePoint, const glm::vec3& planeNormal)
{
    return glm::dot(point - planePoint, glm::normalize(planeNormal));
}

Mesh::Vertex InterpolateVertex(const Mesh::Vertex& a, const Mesh::Vertex& b, float t)
{
    Mesh::Vertex mixed{};
    for (int i = 0; i < 3; ++i)
    {
        mixed.pos[i] = a.pos[i] + (b.pos[i] - a.pos[i]) * t;
        mixed.normal[i] = a.normal[i] + (b.normal[i] - a.normal[i]) * t;
        if (i < 2)
        {
            mixed.uv[i] = a.uv[i] + (b.uv[i] - a.uv[i]) * t;
        }
    }
    for (int i = 0; i < 4; ++i)
    {
        mixed.tangent[i] = a.tangent[i] + (b.tangent[i] - a.tangent[i]) * t;
    }
    for (int i = 0; i < 2; ++i)
    {
        mixed.uv1[i] = a.uv1[i] + (b.uv1[i] - a.uv1[i]) * t;
    }
    for (int i = 0; i < 4; ++i)
    {
        mixed.color[i] = a.color[i] + (b.color[i] - a.color[i]) * t;
    }
    for (int i = 0; i < 4; ++i)
    {
        mixed.joints0[i] = a.joints0[i] + (b.joints0[i] - a.joints0[i]) * t;
        mixed.weights0[i] = a.weights0[i] + (b.weights0[i] - a.weights0[i]) * t;
        mixed.joints1[i] = a.joints1[i] + (b.joints1[i] - a.joints1[i]) * t;
        mixed.weights1[i] = a.weights1[i] + (b.weights1[i] - a.weights1[i]) * t;
    }
    return mixed;
}

void AppendTriangulatedPolygon(std::vector<Mesh::Vertex>& output,
    const std::vector<Mesh::Vertex>& polygon)
{
    if (polygon.size() < 3)
        return;
    for (size_t index = 1; index + 1 < polygon.size(); ++index)
    {
        output.push_back(polygon.front());
        output.push_back(polygon[index]);
        output.push_back(polygon[index + 1]);
    }
}

void ClipPolygonToHalfSpace(const std::vector<Mesh::Vertex>& polygon,
    const glm::vec3& planePoint, const glm::vec3& planeNormal,
    bool keepPositiveSide, std::vector<Mesh::Vertex>& output)
{
    output.clear();
    if (polygon.empty())
        return;

    const float epsilon = 1e-5f;
    const glm::vec3 normal = glm::normalize(planeNormal);
    std::vector<Mesh::Vertex> current = polygon;

    for (size_t i = 0; i < current.size(); ++i)
    {
        const Mesh::Vertex& a = current[i];
        const Mesh::Vertex& b = current[(i + 1) % current.size()];
        const glm::vec3 va(a.pos[0], a.pos[1], a.pos[2]);
        const glm::vec3 vb(b.pos[0], b.pos[1], b.pos[2]);
        const float da = SignedDistanceToPlane(va, planePoint, normal);
        const float db = SignedDistanceToPlane(vb, planePoint, normal);
        const bool insideA = keepPositiveSide ? da >= -epsilon : da <= epsilon;
        const bool insideB = keepPositiveSide ? db >= -epsilon : db <= epsilon;

        if (insideA && insideB)
        {
            output.push_back(b);
        }
        else if (insideA && !insideB)
        {
            const float t = std::max(0.f, std::min(1.f, da / (da - db)));
            output.push_back(InterpolateVertex(a, b, t));
        }
        else if (!insideA && insideB)
        {
            const float t = std::max(0.f, std::min(1.f, da / (da - db)));
            output.push_back(InterpolateVertex(a, b, t));
            output.push_back(b);
        }
    }
}

glm::vec3 VertexPosition(const Mesh::Vertex& vertex)
{
    return glm::vec3(vertex.pos[0], vertex.pos[1], vertex.pos[2]);
}

bool SamePosition(const glm::vec3& first, const glm::vec3& second,
    float epsilon = 1e-4f)
{
    const glm::vec3 delta = first - second;
    return glm::dot(delta, delta) <= epsilon * epsilon;
}

struct CutSegment
{
    size_t first = 0;
    size_t second = 0;
};

size_t FindOrAddWeldedCutVertex(std::vector<Mesh::Vertex>& cutVertices,
    const Mesh::Vertex& vertex)
{
    const glm::vec3 position = VertexPosition(vertex);
    for (size_t index = 0; index < cutVertices.size(); ++index)
    {
        if (SamePosition(VertexPosition(cutVertices[index]), position))
            return index;
    }

    cutVertices.push_back(vertex);
    return cutVertices.size() - 1u;
}

void AppendUniqueCutVertex(std::vector<Mesh::Vertex>& vertices,
    const Mesh::Vertex& candidate)
{
    for (const Mesh::Vertex& vertex : vertices)
    {
        if (SamePosition(VertexPosition(vertex), VertexPosition(candidate)))
            return;
    }
    vertices.push_back(candidate);
}

void CollectCutSegment(const std::vector<Mesh::Vertex>& triangle,
    const glm::vec3& planePoint, const glm::vec3& planeNormal,
    std::vector<Mesh::Vertex>& cutVertices, std::vector<CutSegment>& segments)
{
    constexpr float epsilon = 1e-5f;
    std::array<float, 3> distances{};
    bool hasPositive = false;
    bool hasNegative = false;
    for (size_t index = 0; index < triangle.size(); ++index)
    {
        distances[index] = SignedDistanceToPlane(VertexPosition(triangle[index]),
            planePoint, planeNormal);
        hasPositive = hasPositive || distances[index] > epsilon;
        hasNegative = hasNegative || distances[index] < -epsilon;
    }

    // A coplanar edge is already enclosed by its adjacent surface. Only a
    // triangle that crosses the plane contributes a boundary segment.
    if (!hasPositive || !hasNegative)
        return;

    std::vector<Mesh::Vertex> intersections;
    intersections.reserve(2);
    for (size_t index = 0; index < triangle.size(); ++index)
    {
        const size_t next = (index + 1u) % triangle.size();
        const float firstDistance = distances[index];
        const float secondDistance = distances[next];
        if (std::abs(firstDistance) <= epsilon)
            AppendUniqueCutVertex(intersections, triangle[index]);
        if ((firstDistance > epsilon && secondDistance < -epsilon) ||
            (firstDistance < -epsilon && secondDistance > epsilon))
        {
            const float t = firstDistance / (firstDistance - secondDistance);
            AppendUniqueCutVertex(intersections,
                InterpolateVertex(triangle[index], triangle[next], t));
        }
    }

    if (intersections.size() != 2u ||
        SamePosition(VertexPosition(intersections[0]), VertexPosition(intersections[1])))
        return;

    const size_t first = FindOrAddWeldedCutVertex(cutVertices, intersections[0]);
    const size_t second = FindOrAddWeldedCutVertex(cutVertices, intersections[1]);
    if (first != second)
        segments.push_back({ first, second });
}

float Cross2D(const glm::vec2& first, const glm::vec2& second,
    const glm::vec2& third)
{
    const glm::vec2 a = second - first;
    const glm::vec2 b = third - first;
    return a.x * b.y - a.y * b.x;
}

bool PointInTriangle2D(const glm::vec2& point, const glm::vec2& first,
    const glm::vec2& second, const glm::vec2& third)
{
    constexpr float epsilon = 1e-6f;
    const float a = Cross2D(first, second, point);
    const float b = Cross2D(second, third, point);
    const float c = Cross2D(third, first, point);
    return a >= -epsilon && b >= -epsilon && c >= -epsilon;
}

Mesh::Vertex BuildCapVertex(const Mesh::Vertex& boundary,
    const glm::vec3& planePoint, const glm::vec3& tangent,
    const glm::vec3& bitangent, const glm::vec3& capNormal,
    bool normalPointsPositive)
{
    Mesh::Vertex cap = boundary;
    const glm::vec3 relative = VertexPosition(boundary) - planePoint;
    const float u = glm::dot(relative, tangent);
    const float v = glm::dot(relative, bitangent);
    cap.normal[0] = capNormal.x;
    cap.normal[1] = capNormal.y;
    cap.normal[2] = capNormal.z;
    cap.tangent[0] = tangent.x;
    cap.tangent[1] = tangent.y;
    cap.tangent[2] = tangent.z;
    // Tangent-space bitangent must retain the same planar UV orientation on
    // both caps even though their surface normals face opposite directions.
    cap.tangent[3] = normalPointsPositive ? 1.f : -1.f;
    cap.uv[0] = u;
    cap.uv[1] = v;
    cap.uv1[0] = u;
    cap.uv1[1] = v;
    return cap;
}

void AppendCapForLoop(std::vector<Mesh::Vertex>& output,
    const std::vector<Mesh::Vertex>& cutVertices, std::vector<size_t> loop,
    const glm::vec3& planePoint, const glm::vec3& planeNormal,
    bool normalPointsPositive)
{
    if (loop.size() < 3u)
        return;

    const glm::vec3 normal = glm::normalize(planeNormal);
    const glm::vec3 reference = std::abs(normal.z) < 0.999f
        ? glm::vec3(0.f, 0.f, 1.f) : glm::vec3(0.f, 1.f, 0.f);
    const glm::vec3 tangent = glm::normalize(glm::cross(reference, normal));
    const glm::vec3 bitangent = glm::cross(normal, tangent);
    std::vector<glm::vec2> projected(cutVertices.size());
    for (size_t index = 0; index < cutVertices.size(); ++index)
    {
        const glm::vec3 relative = VertexPosition(cutVertices[index]) - planePoint;
        projected[index] = glm::vec2(glm::dot(relative, tangent),
            glm::dot(relative, bitangent));
    }

    // Adjacent source triangles often meet the plane on their shared face
    // diagonal. Remove that collinear seam before triangulating so a planar
    // quad produces two cap triangles instead of a fan of redundant slivers.
    bool removedCollinearPoint = true;
    while (removedCollinearPoint && loop.size() > 3u)
    {
        removedCollinearPoint = false;
        for (size_t index = 0; index < loop.size(); ++index)
        {
            const size_t previous = loop[(index + loop.size() - 1u) % loop.size()];
            const size_t current = loop[index];
            const size_t next = loop[(index + 1u) % loop.size()];
            if (std::abs(Cross2D(projected[previous], projected[current],
                projected[next])) <= 1e-6f)
            {
                loop.erase(loop.begin() + static_cast<std::ptrdiff_t>(index));
                removedCollinearPoint = true;
                break;
            }
        }
    }

    float signedArea = 0.f;
    for (size_t index = 0; index < loop.size(); ++index)
    {
        const glm::vec2& first = projected[loop[index]];
        const glm::vec2& second = projected[loop[(index + 1u) % loop.size()]];
        signedArea += first.x * second.y - first.y * second.x;
    }
    if (std::abs(signedArea) <= 1e-6f)
        return;
    if (signedArea < 0.f)
        std::reverse(loop.begin(), loop.end());

    const glm::vec3 capNormal = normalPointsPositive ? normal : -normal;
    std::vector<size_t> remaining = loop;
    // Ear clipping handles concave cross-sections, unlike a fan from one cut
    // point. The working winding is counter-clockwise in the plane basis.
    while (remaining.size() > 2u)
    {
        bool foundEar = false;
        for (size_t index = 0; index < remaining.size(); ++index)
        {
            const size_t previous = remaining[(index + remaining.size() - 1u) % remaining.size()];
            const size_t current = remaining[index];
            const size_t next = remaining[(index + 1u) % remaining.size()];
            if (Cross2D(projected[previous], projected[current], projected[next]) <= 1e-6f)
                continue;

            bool containsVertex = false;
            for (const size_t candidate : remaining)
            {
                if (candidate == previous || candidate == current || candidate == next)
                    continue;
                if (PointInTriangle2D(projected[candidate], projected[previous],
                    projected[current], projected[next]))
                {
                    containsVertex = true;
                    break;
                }
            }
            if (containsVertex)
                continue;

            const Mesh::Vertex a = BuildCapVertex(cutVertices[previous], planePoint,
                tangent, bitangent, capNormal, normalPointsPositive);
            const Mesh::Vertex b = BuildCapVertex(cutVertices[current], planePoint,
                tangent, bitangent, capNormal, normalPointsPositive);
            const Mesh::Vertex c = BuildCapVertex(cutVertices[next], planePoint,
                tangent, bitangent, capNormal, normalPointsPositive);
            if (normalPointsPositive)
            {
                output.push_back(a);
                output.push_back(b);
                output.push_back(c);
            }
            else
            {
                output.push_back(a);
                output.push_back(c);
                output.push_back(b);
            }
            remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(index));
            foundEar = true;
            break;
        }

        // An open/self-intersecting cut must not receive a malformed cap.
        if (!foundEar)
            return;
    }
}

void AppendCutCaps(std::vector<Mesh::Vertex>& front,
    std::vector<Mesh::Vertex>& back, const std::vector<Mesh::Vertex>& cutVertices,
    const std::vector<CutSegment>& segments, const glm::vec3& planePoint,
    const glm::vec3& planeNormal)
{
    if (cutVertices.empty() || segments.empty())
        return;

    std::vector<bool> consumed(segments.size(), false);
    for (size_t segmentIndex = 0; segmentIndex < segments.size(); ++segmentIndex)
    {
        if (consumed[segmentIndex])
            continue;

        const CutSegment& firstSegment = segments[segmentIndex];
        std::vector<size_t> loop{ firstSegment.first, firstSegment.second };
        consumed[segmentIndex] = true;
        size_t current = firstSegment.second;
        bool closed = false;
        while (true)
        {
            if (current == loop.front())
            {
                closed = true;
                break;
            }

            size_t nextSegment = segments.size();
            for (size_t candidate = 0; candidate < segments.size(); ++candidate)
            {
                if (consumed[candidate])
                    continue;
                if (segments[candidate].first == current || segments[candidate].second == current)
                {
                    nextSegment = candidate;
                    break;
                }
            }
            if (nextSegment == segments.size())
                break;

            consumed[nextSegment] = true;
            const CutSegment& segment = segments[nextSegment];
            current = segment.first == current ? segment.second : segment.first;
            loop.push_back(current);
            if (loop.size() > segments.size() + 1u)
                break;
        }

        if (!closed || loop.size() < 4u)
            continue;
        loop.pop_back();
        // The positive half is closed toward negative plane normal; the
        // negative half is closed toward positive plane normal.
        AppendCapForLoop(front, cutVertices, loop, planePoint, planeNormal, false);
        AppendCapForLoop(back, cutVertices, loop, planePoint, planeNormal, true);
    }
}
}

Mesh::SliceResult Mesh::SliceByPlane(const std::vector<Vertex>& vertices,
    const glm::vec3& planePoint, const glm::vec3& planeNormal)
{
    if (vertices.size() < 3)
        return {{}, {}};

    std::vector<Vertex> front;
    std::vector<Vertex> back;
    std::vector<Vertex> cutVertices;
    std::vector<CutSegment> cutSegments;
    const glm::vec3 normal = glm::normalize(planeNormal);

    for (size_t index = 0; index < vertices.size(); index += 3)
    {
        std::vector<Vertex> triangle;
        triangle.reserve(3);
        for (size_t offset = 0; offset < 3; ++offset)
        {
            const size_t vertexIndex = index + offset;
            if (vertexIndex >= vertices.size())
                break;
            triangle.push_back(vertices[vertexIndex]);
        }
        if (triangle.size() != 3)
            continue;

        std::vector<Vertex> positiveSide;
        std::vector<Vertex> negativeSide;
        ClipPolygonToHalfSpace(triangle, planePoint, normal, true, positiveSide);
        ClipPolygonToHalfSpace(triangle, planePoint, normal, false, negativeSide);

        AppendTriangulatedPolygon(front, positiveSide);
        AppendTriangulatedPolygon(back, negativeSide);
        CollectCutSegment(triangle, planePoint, normal, cutVertices, cutSegments);
    }

    AppendCutCaps(front, back, cutVertices, cutSegments, planePoint, normal);

    return { front, back };
}

#pragma region DX12 buffer creation and rendering

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
        m_vertexBuffer = bufferFactory->CreateBuffer(
            IGraphicsBuffer::Usage::VertexBuffer,
            IGraphicsBuffer::AccessMode::Upload, vertexBytes,
            m_terrainVertices.data());
        m_indexBuffer = bufferFactory->CreateBuffer(
            IGraphicsBuffer::Usage::IndexBuffer,
            IGraphicsBuffer::AccessMode::Upload, indexBytes, m_indices.data());
        if (!m_vertexBuffer || !m_indexBuffer)
            throw std::runtime_error("Failed to create indexed terrain buffers");
        m_ready = true;
        return;
    }
    const uint64_t byteSize = m_vertices.size() * sizeof(Vertex);

    // Create upload buffer through the graphics factory
    m_vertexBuffer = bufferFactory->CreateBuffer(
        IGraphicsBuffer::Usage::VertexBuffer,
        IGraphicsBuffer::AccessMode::Upload,
        byteSize,
        m_vertices.data());

    if (!m_vertexBuffer)
        throw std::runtime_error("Failed to create vertex buffer");

    CreateMorphBuffers();
    m_ready = true;
}
#pragma endregion

bool Mesh::DrawProperties(::Engine::Editor::IEditorUi& ui)
{
    ui.ValueLabel("Asset", m_filePath.empty() ? "(generated mesh)" : m_filePath.c_str());
    const std::string vertexCount = std::to_string(GetVertexCount());
    const std::string triangleCount = std::to_string(
        m_indices.empty() ? m_vertices.size() / 3 : m_indices.size() / 3);
    ui.ValueLabel("Vertices", vertexCount.c_str());
    ui.ValueLabel("Triangles", triangleCount.c_str());
    ui.ValueLabel("GPU Buffer", m_ready ? "Ready" : "Not prepared");

    if (m_hasBounds)
    {
        char minimum[96]{}, maximum[96]{};
        std::snprintf(minimum, sizeof(minimum), "%.3f, %.3f, %.3f",
            m_boundsMin.x, m_boundsMin.y, m_boundsMin.z);
        std::snprintf(maximum, sizeof(maximum), "%.3f, %.3f, %.3f",
            m_boundsMax.x, m_boundsMax.y, m_boundsMax.z);
        ui.ValueLabel("Bounds Minimum", minimum);
        ui.ValueLabel("Bounds Maximum", maximum);
    }

    const std::string morphCount = std::to_string(m_morphTargets.size());
    ui.ValueLabel("Morph Targets", morphCount.c_str());
    bool changed = false;
    if (!m_morphWeights.empty() &&
        ui.CollapsingHeader("Morph Weights", false))
    {
        for (size_t index = 0; index < m_morphWeights.size(); ++index)
        {
            ui.PushId(&m_morphWeights[index]);
            const std::string label = "Target " + std::to_string(index);
            changed = ui.DragFloat(label.c_str(), &m_morphWeights[index],
                0.01f, -1.f, 1.f) || changed;
            ui.PopId();
        }
    }
    if (changed)
    {
        SyncMorphWeights();
    }
    return changed;
}

Mesh::JsonValue Mesh::Serialize() const
{
    JsonValue result = Component::Serialize();
    if (m_morphTargets.empty()) return result;
    result.Set("morphNodeIndex", JsonValue(static_cast<int>(m_morphNodeIndex)));
    result.Set("morphWeights", FloatArray(m_morphWeights));
    JsonValue targets = JsonValue::MakeArray();
    for (const MorphTarget& target : m_morphTargets)
        targets.Push(JsonValue::MakeObject()
            .Set("positions", Vec3Array(target.positions))
            .Set("normals", Vec3Array(target.normals))
            .Set("tangents", Vec3Array(target.tangents)));
    return result.Set("morphTargets", std::move(targets));
}

void Mesh::DeserializeLegacyMorphTargets(const JsonValue& value)
{
    std::vector<MorphTarget> targets;
    const JsonValue& list = value["targets"];
    for (size_t i = 0; i < list.ArraySize(); ++i)
    {
        MorphTarget target;
        target.positions = ReadVec3Array(list.ArrayAt(i)["positions"]);
        target.normals = ReadVec3Array(list.ArrayAt(i)["normals"]);
        target.tangents = ReadVec3Array(list.ArrayAt(i)["tangents"]);
        targets.push_back(std::move(target));
    }
    std::vector<float> weights;
    for (size_t i = 0; i < value["weights"].ArraySize(); ++i)
        weights.push_back(value["weights"].ArrayAt(i).AsFloat());
    SetMorphData(static_cast<unsigned>(value["nodeIndex"].AsInt()),
        std::move(targets), std::move(weights));
}

void Mesh::Deserialize(const JsonValue& v)
{
    if (v.Has("file"))
        LoadFromFile(v["file"].AsString());
    if (v.Has("morphTargets"))
    {
        JsonValue legacy = JsonValue::MakeObject()
            .Set("nodeIndex", v["morphNodeIndex"])
            .Set("weights", v["morphWeights"])
            .Set("targets", v["morphTargets"]);
        DeserializeLegacyMorphTargets(legacy);
    }
}

void Mesh::OnAfterDeserialize(IGraphicsProvider* graphicsProvider)
{
    if (graphicsProvider)
        CreateBuffer(graphicsProvider->GetBufferFactory());
}
}
