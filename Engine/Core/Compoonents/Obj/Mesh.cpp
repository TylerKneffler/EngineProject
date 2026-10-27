#include "Mesh.h"
#include "Core/Model/MeshPackAsset.h"
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
#include <chrono>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <glm/gtc/quaternion.hpp>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace Engine::Components
{
Mesh::Mesh()
{
    SetTypeName(COMPONENT_TYPE_NAME(Mesh));
    RegisterField("file", m_filePath);
    RegisterField("useLightProbes",useLightProbes,"Lighting");
}

#ifndef ENGINE_ASSETS_PATH
#define ENGINE_ASSETS_PATH "Engine/Core/Assets/"
#endif

std::string Mesh::ResolveFilePath(const std::string& path)
{
    const std::filesystem::path requested(path);
    if (std::filesystem::exists(requested))
        return requested.lexically_normal().generic_string();

    // Bundled assets may carry absolute paths from the checkout that authored
    // them. Resolve only paths under the built-in asset root in this checkout.
    const std::string generic = requested.generic_string();
    constexpr std::string_view marker = "Engine/Core/Assets/";
    const size_t root = generic.find(marker);
    if (root != std::string::npos)
    {
        const std::filesystem::path relocated =
            std::filesystem::path(ENGINE_ASSETS_PATH) /
            generic.substr(root + marker.size());
        if (std::filesystem::exists(relocated))
            return relocated.lexically_normal().generic_string();
    }

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
constexpr uint32_t kNativeMeshVersion = 7;
constexpr float kPi = 3.14159265358979323846f;
struct LegacyVertexV2
{
    float pos[3], normal[3], uv[2], tangent[4];
};
struct LegacyVertexV3
{
    float pos[3], normal[3], uv[2], tangent[4], uv1[2], color[4];
};
struct LegacyVertexV5
{
    float pos[3], normal[3], uv[2], tangent[4], uv1[2], color[4];
    float joints0[4], weights0[4], joints1[4], weights1[4];
};
static_assert(sizeof(LegacyVertexV5) == 136);
struct LegacyVertexV6
{
    LegacyVertexV5 base;
    float uvChannels[5][4];
};
static_assert(sizeof(LegacyVertexV6) == 216);

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

Engine::Serialization::JsonValue Vec4Array(const std::vector<glm::vec4>& values)
{
    auto result = Engine::Serialization::JsonValue::MakeArray();
    for (const glm::vec4& value : values)
        result.Push(Engine::Serialization::JsonValue::MakeArray()
            .Push(Engine::Serialization::JsonValue(value.x))
            .Push(Engine::Serialization::JsonValue(value.y))
            .Push(Engine::Serialization::JsonValue(value.z))
            .Push(Engine::Serialization::JsonValue(value.w)));
    return result;
}

std::vector<glm::vec4> ReadVec4Array(const Engine::Serialization::JsonValue& value)
{
    std::vector<glm::vec4> result;
    for (size_t i = 0; i < value.ArraySize(); ++i)
    {
        const auto& item = value.ArrayAt(i);
        result.emplace_back(item.ArrayAt(0).AsFloat(), item.ArrayAt(1).AsFloat(),
            item.ArrayAt(2).AsFloat(), item.ArrayAt(3).AsFloat());
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

static size_t ResolveObjIndex(int index, size_t count,
    const char* semantic, const std::string& path)
{
    // OBJ indices are one-based; negative values address backward from the
    // elements defined so far. Zero is never valid.
    const int64_t resolved = index > 0
        ? static_cast<int64_t>(index) - 1
        : (index < 0 ? static_cast<int64_t>(count) + index : -1);
    if (resolved < 0 || resolved >= static_cast<int64_t>(count))
    {
        throw std::runtime_error("Mesh: invalid OBJ " +
            std::string(semantic) + " index in " + path);
    }
    return static_cast<size_t>(resolved);
}
#pragma endregion
#pragma region Mesh implementation

void Mesh::LoadFromFile(const std::string& path)
{
    ClearSurfaceOffsets();
    MarkConfigurationDirty();
    ++m_authoredGeometryRevision;
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
        auto nativeFile = Engine::Model::MeshPackAsset::Open(resolvedPath);
        if (!nativeFile)
            throw std::runtime_error("Mesh: failed to open native mesh: " + path);
        std::istream& native = *nativeFile;
        uint32_t magic = 0, version = 0, count = 0;
        native.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        native.read(reinterpret_cast<char*>(&version), sizeof(version));
        native.read(reinterpret_cast<char*>(&count), sizeof(count));
        if (!native || magic != kNativeMeshMagic ||
            (version != 2 && version != 3 && version != 4 && version != 5 &&
                version != 6 &&
                version != kNativeMeshVersion))
            throw std::runtime_error("Mesh: invalid native mesh: " + path);
        uint32_t indexCount = 0;
        if (version >= 5)
            native.read(reinterpret_cast<char*>(&indexCount), sizeof(indexCount));
        if (!native || count == 0 || count > 100000000u ||
            indexCount > 300000000u)
            throw std::runtime_error("Mesh: invalid native mesh size: " + path);
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
        else if (version <= 5)
        {
            std::vector<LegacyVertexV5> legacy(count);
            native.read(reinterpret_cast<char*>(legacy.data()),
                static_cast<std::streamsize>(legacy.size() * sizeof(LegacyVertexV5)));
            for (size_t i = 0; i < legacy.size(); ++i)
                std::memcpy(m_vertices[i].pos, legacy[i].pos, sizeof(LegacyVertexV5));
        }
        else if (version == 6)
        {
            std::vector<LegacyVertexV6> legacy(count);
            native.read(reinterpret_cast<char*>(legacy.data()),
                static_cast<std::streamsize>(legacy.size() * sizeof(LegacyVertexV6)));
            for (size_t i = 0; i < legacy.size(); ++i)
                std::memcpy(m_vertices[i].pos, &legacy[i], sizeof(LegacyVertexV6));
        }
        else
            native.read(reinterpret_cast<char*>(m_vertices.data()),
                static_cast<std::streamsize>(m_vertices.size() * sizeof(Vertex)));
        if (version >= 5)
        {
            m_indices.resize(indexCount);
            native.read(reinterpret_cast<char*>(m_indices.data()),
                static_cast<std::streamsize>(m_indices.size() * sizeof(uint32_t)));
        }
        if (version <= 5)
            for (Vertex& vertex : m_vertices)
            {
                std::copy(std::begin(vertex.uv), std::end(vertex.uv), vertex.uvChannels[0]);
                std::copy(std::begin(vertex.uv1), std::end(vertex.uv1), vertex.uvChannels[1]);
            }
        if (!native)
            throw std::runtime_error("Mesh: truncated native mesh: " + path);
        if (!ValidateAuthoredGeometry(m_vertices, m_indices))
            throw std::runtime_error("Mesh: invalid native mesh geometry: " + path);
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
            std::vector<Vertex> faceVertices;
            std::string faceToken;
            while (ss >> faceToken)
            {
                int vi = 0, vti = 0, vni = 0;
                ParseFaceToken(faceToken, vi, vti, vni);
                Vertex v{};
                const size_t positionIndex = ResolveObjIndex(
                    vi, positions.size(), "position", path);
                const auto& p = positions[positionIndex];
                v.pos[0] = p[0]; v.pos[1] = p[1]; v.pos[2] = p[2];
                const auto& color = colors[positionIndex];
                std::copy(color.begin(), color.end(), v.color);
                if (vti != 0)
                {
                    const auto& uv = texcoords[ResolveObjIndex(
                        vti, texcoords.size(), "texture-coordinate", path)];
                    v.uv[0] = uv[0]; v.uv[1] = uv[1];
                }
                if (vni != 0)
                {
                    const auto& n = normals[ResolveObjIndex(
                        vni, normals.size(), "normal", path)];
                    v.normal[0] = n[0]; v.normal[1] = n[1]; v.normal[2] = n[2];
                }
                faceVertices.push_back(v);
            }

            // Preserve authored winding while triangulating the complete
            // polygon. The old three-token parser discarded every vertex
            // after the third, leaving a diagonal hole in every quad.
            for (size_t corner = 1; corner + 1 < faceVertices.size(); ++corner)
            {
                m_vertices.push_back(faceVertices[0]);
                m_vertices.push_back(faceVertices[corner]);
                m_vertices.push_back(faceVertices[corner + 1]);
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
    ClearSurfaceOffsets();

    // A real portal cut adds intersection vertices, so the two clipped halves
    // generally contain more vertices than the original mesh. Recreate the
    // upload buffer when its size changes instead of rejecting the cut.
    m_terrainVertices.clear();
    m_indices.clear();
    m_indexBuffer.reset();
    const bool canReuseBuffer = m_vertexBuffer &&
        m_vertexBuffer->GetAccessMode() == IGraphicsBuffer::AccessMode::Upload &&
        byteSize <= m_vertexBuffer->GetSize();
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
    ClearSurfaceOffsets();

    m_terrainVertices.clear();
    m_indices.clear();
    m_indexBuffer.reset();
    const bool canReuseBuffer = m_vertexBuffer &&
        m_vertexBuffer->GetAccessMode() == IGraphicsBuffer::AccessMode::Upload &&
        byteSize <= m_vertexBuffer->GetSize();
    if (!canReuseBuffer && (m_vertexBuffer || m_bufferFactory))
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
    ClearSurfaceOffsets();
    m_terrainVertices.clear();
    m_indices.clear();
    m_indexBuffer.reset();
    m_vertices = std::move(vertices);
    ++m_authoredGeometryRevision;
    MarkConfigurationDirty();
    UpdateBounds();
    if (m_bufferFactory)
        CreateBuffer(m_bufferFactory);
    return true;
}

bool Mesh::UpdateAuthoredVertices(std::vector<Vertex> vertices)
{
    if (vertices.size() != m_vertices.size() || !m_terrainVertices.empty() ||
        !ValidateAuthoredGeometry(vertices, m_indices)) return false;
    const size_t byteSize = vertices.size() * sizeof(Vertex);
    if (m_vertexBuffer)
    {
        if (byteSize > m_vertexBuffer->GetSize()) return false;
        if (m_vertexBuffer->GetAccessMode() != IGraphicsBuffer::AccessMode::Upload)
        {
            if (!m_bufferFactory) return false;
            auto replacement = m_bufferFactory->CreateBuffer(
                IGraphicsBuffer::Usage::VertexBuffer,
                IGraphicsBuffer::AccessMode::Upload, byteSize, vertices.data());
            if (!replacement) return false;
            m_vertexBuffer = std::move(replacement);
        }
        else
        {
            void* mapped = m_vertexBuffer->Map();
            if (!mapped) return false;
            std::memcpy(mapped, vertices.data(), byteSize);
            m_vertexBuffer->Unmap();
        }
    }
    ClearSurfaceOffsets();
    m_vertices = std::move(vertices);
    ++m_authoredGeometryRevision;
    MarkConfigurationDirty();
    UpdateBounds();
    if (!m_vertexBuffer && m_bufferFactory) CreateBuffer(m_bufferFactory);
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
    ClearSurfaceOffsets();
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
    ClearSurfaceOffsets();
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

void Mesh::SetMorphData(unsigned nodeIndex, std::vector<MorphTarget> targets,
    std::vector<float> weights)
{
    m_morphFilePath.clear();
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

bool Mesh::SaveMorphFile(const std::string& path,
    const std::vector<MorphTarget>& targets)
{
    static_assert(sizeof(glm::vec3) == sizeof(float) * 3);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    const uint32_t magic = 0x46524f4du; // MORF
    const uint32_t version = 3;
    const uint32_t count = static_cast<uint32_t>(targets.size());
    file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));
    for (const auto& target : targets)
    {
        const uint32_t nameSize = static_cast<uint32_t>(target.name.size());
        file.write(reinterpret_cast<const char*>(&nameSize), sizeof(nameSize));
        file.write(target.name.data(), nameSize);
        for (const auto* values : { &target.positions, &target.normals,
                &target.tangents })
        {
            const uint32_t size = static_cast<uint32_t>(values->size());
            file.write(reinterpret_cast<const char*>(&size), sizeof(size));
            file.write(reinterpret_cast<const char*>(values->data()),
                static_cast<std::streamsize>(size * sizeof(glm::vec3)));
        }
        const uint32_t uvSize = static_cast<uint32_t>(target.uv.size());
        file.write(reinterpret_cast<const char*>(&uvSize), sizeof(uvSize));
        file.write(reinterpret_cast<const char*>(target.uv.data()),
            static_cast<std::streamsize>(uvSize * sizeof(glm::vec4)));
        for (const auto& channel : target.uvChannels)
        {
            const uint32_t size = static_cast<uint32_t>(channel.size());
            file.write(reinterpret_cast<const char*>(&size), sizeof(size));
            file.write(reinterpret_cast<const char*>(channel.data()),
                static_cast<std::streamsize>(size * sizeof(glm::vec4)));
        }
    }
    return file.good();
}

bool Mesh::LoadMorphFile(const std::string& path, unsigned nodeIndex,
    std::vector<float> weights)
{
    static_assert(sizeof(glm::vec3) == sizeof(float) * 3);
    auto morphFile = Engine::Model::MeshPackAsset::Open(ResolveFilePath(path));
    if (!morphFile) return false;
    std::istream& file = *morphFile;
    uint32_t magic = 0, version = 0, count = 0;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    file.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (!file || magic != 0x46524f4du || (version != 1 && version != 2 && version != 3) ||
        count > 10000u)
        return false;
    std::vector<MorphTarget> targets;
    targets.reserve(count);
    for (uint32_t index = 0; index < count; ++index)
    {
        MorphTarget target;
        uint32_t nameSize = 0;
        file.read(reinterpret_cast<char*>(&nameSize), sizeof(nameSize));
        if (!file || nameSize > 4096u) return false;
        target.name.resize(nameSize);
        file.read(target.name.data(), nameSize);
        for (auto* values : { &target.positions, &target.normals,
                &target.tangents })
        {
            uint32_t size = 0;
            file.read(reinterpret_cast<char*>(&size), sizeof(size));
            if (!file || size > 100000000u ||
                (!m_vertices.empty() && size != 0 && size != m_vertices.size()))
                return false;
            values->resize(size);
            file.read(reinterpret_cast<char*>(values->data()),
                static_cast<std::streamsize>(size * sizeof(glm::vec3)));
            if (!file) return false;
        }
        if (version >= 2)
        {
            uint32_t size = 0;
            file.read(reinterpret_cast<char*>(&size), sizeof(size));
            if (!file || size > 100000000u ||
                (!m_vertices.empty() && size != 0 && size != m_vertices.size()))
                return false;
            target.uv.resize(size);
            file.read(reinterpret_cast<char*>(target.uv.data()),
                static_cast<std::streamsize>(size * sizeof(glm::vec4)));
            if (!file) return false;
        }
        if (version >= 3)
            for (auto& channel : target.uvChannels)
            {
                uint32_t size = 0;
                file.read(reinterpret_cast<char*>(&size), sizeof(size));
                if (!file || size > 100000000u ||
                    (!m_vertices.empty() && size != 0 && size != m_vertices.size()))
                    return false;
                channel.resize(size);
                file.read(reinterpret_cast<char*>(channel.data()),
                    static_cast<std::streamsize>(size * sizeof(glm::vec4)));
                if (!file) return false;
            }
        targets.push_back(std::move(target));
    }
    SetMorphData(nodeIndex, std::move(targets), std::move(weights));
    m_morphFilePath = path;
    return true;
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
    m_activeMorphTargetCount = 0;
    if (!m_bufferFactory || m_vertices.empty() || m_morphTargets.empty())
        return;

    const size_t vertexCount = m_vertices.size();
    const bool hasFullUv = std::any_of(m_morphTargets.begin(),
        m_morphTargets.end(), [](const MorphTarget& target)
        {
            for (size_t channel = 0; channel < target.uvChannels.size(); ++channel)
            {
                const auto& values = target.uvChannels[channel];
                if (channel >= 2 && !values.empty()) return true;
                if (std::any_of(values.begin(), values.end(), [](const glm::vec4& value)
                    { return value.z != 0.f || value.w != 0.f; })) return true;
            }
            return false;
        });
    const bool hasUv = std::any_of(m_morphTargets.begin(),
        m_morphTargets.end(), [](const MorphTarget& target)
        { return !target.uv.empty(); });
    const bool hasNormalOrTangent = std::any_of(m_morphTargets.begin(),
        m_morphTargets.end(), [](const MorphTarget& target)
        { return !target.normals.empty() || !target.tangents.empty(); });
    m_morphDeltaStrideFloat4 = hasFullUv ? 9u : hasUv ? 4u :
        hasNormalOrTangent ? 3u : 1u;
    std::vector<glm::vec4> deltas(vertexCount * m_morphTargets.size() *
        m_morphDeltaStrideFloat4, glm::vec4(0.f));
    for (size_t targetIndex = 0; targetIndex < m_morphTargets.size(); ++targetIndex)
    {
        const MorphTarget& target = m_morphTargets[targetIndex];
        for (size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
        {
            const size_t base = (targetIndex * vertexCount + vertexIndex) *
                m_morphDeltaStrideFloat4;
            if (vertexIndex < target.positions.size())
                deltas[base] = glm::vec4(
                    target.positions[vertexIndex], 0.f);
            if (m_morphDeltaStrideFloat4 >= 3 &&
                vertexIndex < target.normals.size())
                deltas[base + 1] = glm::vec4(
                    target.normals[vertexIndex], 0.f);
            if (m_morphDeltaStrideFloat4 >= 3 &&
                vertexIndex < target.tangents.size())
                deltas[base + 2] = glm::vec4(
                    target.tangents[vertexIndex], 0.f);
            if (m_morphDeltaStrideFloat4 >= 4 && vertexIndex < target.uv.size())
                deltas[base + 3] = target.uv[vertexIndex];
            if (hasFullUv)
                for (size_t channel = 0; channel < target.uvChannels.size(); ++channel)
                    if (vertexIndex < target.uvChannels[channel].size())
                        deltas[base + 4 + channel] = target.uvChannels[channel][vertexIndex];
        }
    }
    m_morphDeltaBuffer = m_bufferFactory->CreateStaticShaderResource(
        static_cast<uint64_t>(deltas.size()) * sizeof(glm::vec4),
        deltas.data(), sizeof(glm::vec4));

    // Each float4 stores the authored target index and its current weight.
    // The vertex shaders can then skip inactive targets entirely.
    m_packedMorphWeights.assign(m_morphTargets.size(), glm::vec4(0.f));
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
    m_activeMorphTargetCount = 0;
    for (size_t index = 0; index < m_morphWeights.size() &&
        index < m_morphTargets.size(); ++index)
        if (m_morphWeights[index] != 0.f)
            m_packedMorphWeights[m_activeMorphTargetCount++] = glm::vec4(
                static_cast<float>(index), m_morphWeights[index], 0.f, 0.f);
    if (void* mapped = m_morphWeightBuffer->Map())
    {
        const uint64_t byteCount = static_cast<uint64_t>(m_activeMorphTargetCount) *
            sizeof(glm::vec4);
        std::memcpy(mapped, m_packedMorphWeights.data(), static_cast<size_t>(byteCount));
        m_morphWeightBuffer->Unmap();
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
    for (int channel = 0; channel < 5; ++channel)
        for (int component = 0; component < 4; ++component)
            mixed.uvChannels[channel][component] = a.uvChannels[channel][component] +
                (b.uvChannels[channel][component] - a.uvChannels[channel][component]) * t;
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

std::vector<Mesh::Vertex> Mesh::BuildPosedVertices(
    const std::vector<glm::mat4>* skinPalette,
    bool includeSurfaceOffsets) const
{
    std::vector<Vertex> deformed = m_vertices;
    for (size_t targetIndex = 0; targetIndex < m_morphTargets.size() &&
        targetIndex < m_morphWeights.size(); ++targetIndex)
    {
        const float weight = m_morphWeights[targetIndex];
        const MorphTarget& target = m_morphTargets[targetIndex];
        if (std::abs(weight) <= 1e-7f) continue;
        for (size_t index = 0; index < deformed.size(); ++index)
        {
            Vertex& vertex = deformed[index];
            if (index < target.positions.size())
            {
                vertex.pos[0] += target.positions[index].x * weight;
                vertex.pos[1] += target.positions[index].y * weight;
                vertex.pos[2] += target.positions[index].z * weight;
            }
            if (index < target.normals.size())
            {
                vertex.normal[0] += target.normals[index].x * weight;
                vertex.normal[1] += target.normals[index].y * weight;
                vertex.normal[2] += target.normals[index].z * weight;
            }
            if (index < target.tangents.size())
            {
                vertex.tangent[0] += target.tangents[index].x * weight;
                vertex.tangent[1] += target.tangents[index].y * weight;
                vertex.tangent[2] += target.tangents[index].z * weight;
            }
            if (index < target.uv.size())
            {
                vertex.uv[0] += target.uv[index].x * weight;
                vertex.uv[1] += target.uv[index].y * weight;
                vertex.uv1[0] += target.uv[index].z * weight;
                vertex.uv1[1] += target.uv[index].w * weight;
            }
            for (size_t channel = 0; channel < target.uvChannels.size(); ++channel)
                if (index < target.uvChannels[channel].size())
                {
                    const glm::vec4 delta = target.uvChannels[channel][index] * weight;
                    for (size_t component = channel < 2 ? 2 : 0; component < 4; ++component)
                        vertex.uvChannels[channel][component] += delta[static_cast<glm::length_t>(component)];
                }
            vertex.uvChannels[0][0] = vertex.uv[0];
            vertex.uvChannels[0][1] = vertex.uv[1];
            vertex.uvChannels[1][0] = vertex.uv1[0];
            vertex.uvChannels[1][1] = vertex.uv1[1];
        }
    }
    if (skinPalette && !skinPalette->empty())
    {
        for (Vertex& vertex : deformed)
        {
            const glm::vec3 sourcePosition(vertex.pos[0], vertex.pos[1], vertex.pos[2]);
            const glm::vec3 sourceNormal(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
            const glm::vec3 sourceTangent(vertex.tangent[0], vertex.tangent[1], vertex.tangent[2]);
            const int deformMode = static_cast<int>(vertex.sdefCenterAndMode[3]);
            glm::vec3 specialPosition(0.f), specialNormal(0.f), specialTangent(0.f);
            bool special = false;
            if (deformMode == 3)
            {
                const auto joint0 = static_cast<uint32_t>(vertex.joints0[0]);
                const auto joint1 = static_cast<uint32_t>(vertex.joints0[1]);
                if (joint0 < skinPalette->size() && joint1 < skinPalette->size())
                {
                    const glm::mat4& bone0 = (*skinPalette)[joint0];
                    const glm::mat4& bone1 = (*skinPalette)[joint1];
                    const float weight = vertex.weights0[0];
                    const glm::vec3 center(vertex.sdefCenterAndMode[0],
                        vertex.sdefCenterAndMode[1], vertex.sdefCenterAndMode[2]);
                    const glm::vec3 radius0(vertex.sdefRadius0[0],
                        vertex.sdefRadius0[1], vertex.sdefRadius0[2]);
                    const glm::vec3 radius1(vertex.sdefRadius1[0],
                        vertex.sdefRadius1[1], vertex.sdefRadius1[2]);
                    glm::quat q0 = glm::normalize(glm::quat_cast(bone0));
                    glm::quat q1 = glm::normalize(glm::quat_cast(bone1));
                    if (glm::dot(q0, q1) < 0.f) q1 = -q1;
                    const glm::quat rotation = glm::normalize(glm::slerp(q1, q0, weight));
                    specialPosition = weight * glm::vec3(bone0 * glm::vec4(
                        sourcePosition - center + radius0, 1.f)) +
                        (1.f - weight) * glm::vec3(bone1 * glm::vec4(
                            sourcePosition - center + radius1, 1.f)) +
                        rotation * (center - weight * radius0 -
                            (1.f - weight) * radius1);
                    specialNormal = rotation * sourceNormal;
                    specialTangent = rotation * sourceTangent;
                    special = true;
                }
            }
            else if (deformMode == 4)
            {
                glm::quat real(0.f, 0.f, 0.f, 0.f);
                glm::quat dual(0.f, 0.f, 0.f, 0.f);
                glm::quat reference(1.f, 0.f, 0.f, 0.f);
                bool hasReference = false;
                for (int influence = 0; influence < 4; ++influence)
                {
                    const auto joint = static_cast<uint32_t>(vertex.joints0[influence]);
                    const float weight = vertex.weights0[influence];
                    if (joint >= skinPalette->size() || weight == 0.f) continue;
                    const glm::mat4& bone = (*skinPalette)[joint];
                    glm::quat rotation = glm::normalize(glm::quat_cast(bone));
                    if (!hasReference) { reference = rotation; hasReference = true; }
                    if (glm::dot(reference, rotation) < 0.f) rotation = -rotation;
                    const glm::vec3 translation(bone[3]);
                    const glm::quat displacement = .5f *
                        glm::quat(0.f, translation.x, translation.y, translation.z) * rotation;
                    real += weight * rotation;
                    dual += weight * displacement;
                }
                const float magnitude = glm::length(real);
                if (magnitude > 1e-7f)
                {
                    real /= magnitude;
                    dual /= magnitude;
                    const glm::quat displacement = dual * glm::conjugate(real);
                    specialPosition = real * sourcePosition + 2.f *
                        glm::vec3(displacement.x, displacement.y, displacement.z);
                    specialNormal = real * sourceNormal;
                    specialTangent = real * sourceTangent;
                    special = true;
                }
            }
            if (special)
            {
                const glm::vec3 normal = glm::length(specialNormal) > 1e-7f
                    ? glm::normalize(specialNormal) : specialNormal;
                const glm::vec3 tangent = glm::length(specialTangent) > 1e-7f
                    ? glm::normalize(specialTangent) : specialTangent;
                vertex.pos[0] = specialPosition.x; vertex.pos[1] = specialPosition.y;
                vertex.pos[2] = specialPosition.z;
                vertex.normal[0] = normal.x; vertex.normal[1] = normal.y;
                vertex.normal[2] = normal.z;
                vertex.tangent[0] = tangent.x; vertex.tangent[1] = tangent.y;
                vertex.tangent[2] = tangent.z;
                continue;
            }
            glm::mat4 skin(0.f);
            float totalWeight = 0.f;
            for (int influence = 0; influence < 4; ++influence)
            {
                const uint32_t joint0 = static_cast<uint32_t>(vertex.joints0[influence]);
                const uint32_t joint1 = static_cast<uint32_t>(vertex.joints1[influence]);
                if (joint0 < skinPalette->size())
                {
                    skin += (*skinPalette)[joint0] * vertex.weights0[influence];
                    totalWeight += vertex.weights0[influence];
                }
                if (joint1 < skinPalette->size())
                {
                    skin += (*skinPalette)[joint1] * vertex.weights1[influence];
                    totalWeight += vertex.weights1[influence];
                }
            }
            if (totalWeight <= 1e-7f) continue;
            const glm::vec4 position = skin * glm::vec4(
                vertex.pos[0], vertex.pos[1], vertex.pos[2], 1.f);
            glm::vec3 normal = glm::mat3(skin) * glm::vec3(
                vertex.normal[0], vertex.normal[1], vertex.normal[2]);
            glm::vec3 tangent = glm::mat3(skin) * glm::vec3(
                vertex.tangent[0], vertex.tangent[1], vertex.tangent[2]);
            if (glm::length(normal) > 1e-7f) normal = glm::normalize(normal);
            if (glm::length(tangent) > 1e-7f) tangent = glm::normalize(tangent);
            vertex.pos[0] = position.x; vertex.pos[1] = position.y;
            vertex.pos[2] = position.z;
            vertex.normal[0] = normal.x; vertex.normal[1] = normal.y;
            vertex.normal[2] = normal.z;
            vertex.tangent[0] = tangent.x; vertex.tangent[1] = tangent.y;
            vertex.tangent[2] = tangent.z;
        }
    }
    if (includeSurfaceOffsets && m_surfaceOffsets.size() == deformed.size())
    {
        for (size_t index = 0; index < deformed.size(); ++index)
        {
            const glm::vec3 position(deformed[index].pos[0],
                deformed[index].pos[1], deformed[index].pos[2]);
            const glm::vec3 moved = position + m_surfaceOffsets[index];
            deformed[index].pos[0] = moved.x;
            deformed[index].pos[1] = moved.y;
            deformed[index].pos[2] = moved.z;
        }
        std::vector<glm::vec3> accumulated(deformed.size(), glm::vec3(0.f));
        const size_t triangleCount = m_indices.empty() ? deformed.size() / 3
            : m_indices.size() / 3;
        for (size_t triangle = 0; triangle < triangleCount; ++triangle)
        {
            const size_t a = m_indices.empty() ? triangle * 3 : m_indices[triangle * 3];
            const size_t b = m_indices.empty() ? triangle * 3 + 1 : m_indices[triangle * 3 + 1];
            const size_t c = m_indices.empty() ? triangle * 3 + 2 : m_indices[triangle * 3 + 2];
            if (a >= deformed.size() || b >= deformed.size() || c >= deformed.size()) continue;
            const auto point = [&](size_t i) { return glm::vec3(
                deformed[i].pos[0], deformed[i].pos[1], deformed[i].pos[2]); };
            const glm::vec3 normal = glm::cross(point(b) - point(a), point(c) - point(a));
            accumulated[a] += normal;
            accumulated[b] += normal;
            accumulated[c] += normal;
        }
        for (size_t i = 0; i < deformed.size(); ++i)
            if (glm::dot(accumulated[i], accumulated[i]) > 1e-12f)
            {
                const glm::vec3 normal = glm::normalize(accumulated[i]);
                deformed[i].normal[0] = normal.x;
                deformed[i].normal[1] = normal.y;
                deformed[i].normal[2] = normal.z;
                glm::vec3 tangent(deformed[i].tangent[0], deformed[i].tangent[1],
                    deformed[i].tangent[2]);
                tangent -= normal * glm::dot(normal, tangent);
                if (glm::dot(tangent, tangent) > 1e-12f)
                {
                    tangent = glm::normalize(tangent);
                    deformed[i].tangent[0] = tangent.x;
                    deformed[i].tangent[1] = tangent.y;
                    deformed[i].tangent[2] = tangent.z;
                }
            }
    }
    return deformed;
}

std::vector<Mesh::Vertex> Mesh::BuildPortalCutTriangleStream(
    const std::vector<glm::mat4>* skinPalette) const
{
    std::vector<Vertex> deformed = BuildPosedVertices(skinPalette);
    if (m_indices.empty()) return deformed;
    std::vector<Vertex> triangles;
    triangles.reserve(m_indices.size());
    for (uint32_t index : m_indices)
        if (index < deformed.size()) triangles.push_back(deformed[index]);
    return triangles;
}

void Mesh::SetSurfaceOffsets(std::vector<glm::vec3> offsets,
    const std::vector<glm::mat4>* skinPalette)
{
    if (offsets.size() != m_vertices.size() || offsets.empty())
    {
        ClearSurfaceOffsets();
        return;
    }
    m_surfaceOffsets = std::move(offsets);
    if (!m_bufferFactory) return;
    std::vector<Vertex> posed = BuildPosedVertices(skinPalette);
    const uint64_t bytes = posed.size() * sizeof(Vertex);
    if (!m_posedVertexBuffer || m_posedVertexBuffer->GetSize() != bytes)
        m_posedVertexBuffer = m_bufferFactory->CreateBuffer(
            IGraphicsBuffer::Usage::VertexBuffer,
            IGraphicsBuffer::AccessMode::Upload, bytes, posed.data());
    else if (void* mapped = m_posedVertexBuffer->Map())
    {
        std::memcpy(mapped, posed.data(), static_cast<size_t>(bytes));
        m_posedVertexBuffer->Unmap();
        m_posedVertexBuffer->FlushMappedWrites();
    }
    else
        m_posedVertexBuffer = m_bufferFactory->CreateBuffer(
            IGraphicsBuffer::Usage::VertexBuffer,
            IGraphicsBuffer::AccessMode::Upload, bytes, posed.data());
    if (!m_posedVertexBuffer) m_surfaceOffsets.clear();
}

void Mesh::ClearSurfaceOffsets()
{
    m_surfaceOffsets.clear();
    m_posedVertexBuffer.reset();
}

bool Mesh::SetIndexedGeometry(std::vector<Vertex> vertices,
    std::vector<uint32_t> indices)
{
    if (indices.empty() || !ValidateAuthoredGeometry(vertices, indices))
        return false;
    ClearSurfaceOffsets();
    m_vertices = std::move(vertices);
    m_indices = std::move(indices);
    m_terrainVertices.clear();
    ++m_authoredGeometryRevision;
    MarkConfigurationDirty();
    UpdateBounds();
    if (m_bufferFactory) CreateBuffer(m_bufferFactory);
    return true;
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

    m_vertexBuffer = bufferFactory->CreateStaticVertexBuffer(
        byteSize, m_vertices.data());

    if (!m_vertexBuffer)
        throw std::runtime_error("Failed to create vertex buffer");

    CreateMorphBuffers();
    if (!m_indices.empty())
    {
        const uint64_t indexBytes = static_cast<uint64_t>(m_indices.size()) *
            sizeof(uint32_t);
        m_indexBuffer = bufferFactory->CreateBuffer(
            IGraphicsBuffer::Usage::IndexBuffer,
            IGraphicsBuffer::AccessMode::Upload, indexBytes, m_indices.data());
        if (!m_indexBuffer)
            throw std::runtime_error("Failed to create mesh index buffer");
    }
    else m_indexBuffer.reset();
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
    changed=ui.Checkbox("Use Light Probes",&useLightProbes)||changed;
    if (!m_morphWeights.empty() &&
        ui.PropertyGroupHeader("Morph Weights", false))
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
    if (!m_morphFilePath.empty())
        return result.Set("morphFile", JsonValue(m_morphFilePath));
    JsonValue targets = JsonValue::MakeArray();
    for (const MorphTarget& target : m_morphTargets)
    {
        JsonValue channels = JsonValue::MakeArray();
        for (const auto& channel : target.uvChannels)
            channels.Push(Vec4Array(channel));
        targets.Push(JsonValue::MakeObject()
            .Set("name", JsonValue(target.name))
            .Set("positions", Vec3Array(target.positions))
            .Set("normals", Vec3Array(target.normals))
            .Set("tangents", Vec3Array(target.tangents))
            .Set("uv", Vec4Array(target.uv))
            .Set("uvChannels", std::move(channels)));
    }
    return result.Set("morphTargets", std::move(targets));
}

void Mesh::DeserializeLegacyMorphTargets(const JsonValue& value)
{
    std::vector<MorphTarget> targets;
    const JsonValue& list = value["targets"];
    for (size_t i = 0; i < list.ArraySize(); ++i)
    {
        MorphTarget target;
        if (list.ArrayAt(i).Has("name"))
            target.name = list.ArrayAt(i)["name"].AsString();
        target.positions = ReadVec3Array(list.ArrayAt(i)["positions"]);
        target.normals = ReadVec3Array(list.ArrayAt(i)["normals"]);
        target.tangents = ReadVec3Array(list.ArrayAt(i)["tangents"]);
        target.uv = ReadVec4Array(list.ArrayAt(i)["uv"]);
        const JsonValue& channels = list.ArrayAt(i)["uvChannels"];
        for (size_t channel = 0; channel < target.uvChannels.size() &&
            channel < channels.ArraySize(); ++channel)
            target.uvChannels[channel] = ReadVec4Array(channels.ArrayAt(channel));
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
    if (v.Has("file") && !v["file"].AsString().empty())
        LoadFromFile(v["file"].AsString());
    if (v.Has("morphFile") && !v["morphFile"].AsString().empty())
    {
        std::vector<float> weights;
        for (size_t index = 0; index < v["morphWeights"].ArraySize(); ++index)
            weights.push_back(v["morphWeights"].ArrayAt(index).AsFloat());
        if (!LoadMorphFile(v["morphFile"].AsString(),
                static_cast<unsigned>(v["morphNodeIndex"].AsInt()),
                std::move(weights)))
            throw std::runtime_error("Mesh: invalid morph file: " +
                v["morphFile"].AsString());
    }
    else if (v.Has("morphTargets"))
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
