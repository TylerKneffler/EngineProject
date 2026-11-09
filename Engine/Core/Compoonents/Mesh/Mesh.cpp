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
float Mesh::GetTextureUvWorldScale(uint32_t uvSet) const
{
    if (m_instanceSource) return m_instanceSource->GetTextureUvWorldScale(uvSet);
    if (m_textureUvRevision != m_authoredGeometryRevision)
    {
        m_textureUvScale={0.f,0.f};
        const auto measure=[&](const auto& vertices) {
            const size_t count=m_indices.empty() ? vertices.size():m_indices.size();
            for(size_t triangle=0;triangle+2<count;triangle+=3)
            {
                const size_t ia=m_indices.empty()?triangle:m_indices[triangle];
                const size_t ib=m_indices.empty()?triangle+1:m_indices[triangle+1];
                const size_t ic=m_indices.empty()?triangle+2:m_indices[triangle+2];
                if(std::max({ia,ib,ic})>=vertices.size()) continue;
                const auto& a=vertices[ia];const auto& b=vertices[ib];const auto& c=vertices[ic];
                const glm::vec3 e1(b.pos[0]-a.pos[0],b.pos[1]-a.pos[1],b.pos[2]-a.pos[2]);
                const glm::vec3 e2(c.pos[0]-a.pos[0],c.pos[1]-a.pos[1],c.pos[2]-a.pos[2]);
                for(uint32_t set=0;set<2;++set)
                {
                    const auto uv=[&](const auto& v) {
                        if constexpr(std::is_same_v<std::decay_t<decltype(v)>,Vertex>)
                            if(set) return glm::vec2(v.uv1[0],v.uv1[1]);
                        return glm::vec2(v.uv[0],v.uv[1]);
                    };
                    const glm::vec2 u=uv(b)-uv(a),v=uv(c)-uv(a);
                    const float determinant=u.x*v.y-u.y*v.x;
                    if(std::abs(determinant)<1e-12f) continue;
                    const glm::vec3 du=(e1*v.y-e2*u.y)/determinant;
                    const glm::vec3 dv=(e2*u.x-e1*v.x)/determinant;
                    m_textureUvScale[set]=std::max(m_textureUvScale[set],std::sqrt(glm::dot(du,du)+glm::dot(dv,dv)));
                }
            }
        };
        if(!m_terrainVertices.empty()) measure(m_terrainVertices); else measure(m_vertices);
        for(auto& scale:m_textureUvScale) if(!(scale>0.f) || !std::isfinite(scale)) scale=std::numeric_limits<float>::infinity();
        m_textureUvRevision=m_authoredGeometryRevision;
    }
    return m_textureUvScale[std::min(1u,uvSet)];
}

Mesh::Mesh()
{
    SetTypeName(COMPONENT_TYPE_NAME(Mesh));
    RegisterField("file", m_filePath);
    RegisterField("foliageWindStrength",foliageWindStrength,"Foliage");
    RegisterField("useLightProbes",useLightProbes,"Lighting");
    RegisterField("gpuLodSelection",gpuLodSelection,"LOD");
    RegisterField("gpuLod1Distance",gpuLod1Distance,"LOD");
    RegisterField("screenLodSelection",screenLodSelection,"LOD");
    RegisterField("distanceLodOverride",distanceLodOverride,"LOD");
    RegisterField("screenLod1Pixels",screenLod1Pixels,"LOD");
    RegisterField("screenLod2Pixels",screenLod2Pixels,"LOD");
    RegisterField("screenLodHysteresis",screenLodHysteresis,"LOD");
    RegisterField("gpuLod2Distance",gpuLod2Distance,"LOD");
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

bool Mesh::UsesDeferredGraphicsRetirement() const
{
    return Owner && Owner->GetScene() &&
        Owner->GetScene()->IsDeferredGraphicsRetirementEnabled();
}

void Mesh::ReplaceVertexBuffer(std::shared_ptr<IGraphicsBuffer> buffer)
{
    if (m_vertexBuffer != buffer && Owner && Owner->GetScene())
        Owner->GetScene()->RetireGraphicsBuffer(std::move(m_vertexBuffer));
    m_vertexBuffer = std::move(buffer);
}

void Mesh::ReplaceIndexBuffer(std::shared_ptr<IGraphicsBuffer> buffer)
{
    if (m_indexBuffer != buffer && Owner && Owner->GetScene())
        Owner->GetScene()->RetireGraphicsBuffer(std::move(m_indexBuffer));
    m_indexBuffer = std::move(buffer);
}

void Mesh::ReplacePosedVertexBuffer(std::unique_ptr<IGraphicsBuffer> buffer)
{
    if (Owner && Owner->GetScene())
        Owner->GetScene()->RetireGraphicsBuffer(std::move(m_posedVertexBuffer));
    m_posedVertexBuffer = std::move(buffer);
}

void Mesh::ReplaceMorphDeltaBuffer(std::unique_ptr<IGraphicsBuffer> buffer)
{
    if (Owner && Owner->GetScene())
        Owner->GetScene()->RetireGraphicsBuffer(std::move(m_morphDeltaBuffer));
    m_morphDeltaBuffer = std::move(buffer);
}

void Mesh::ReplaceMorphWeightBuffer(std::unique_ptr<IGraphicsBuffer> buffer)
{
    if (Owner && Owner->GetScene())
        Owner->GetScene()->RetireGraphicsBuffer(std::move(m_morphWeightBuffer));
    m_morphWeightBuffer = std::move(buffer);
}

void Mesh::LoadFromFile(const std::string& path)
{
    m_preparedMorphDeltas.clear();
    ClearInstancing();
    ClearRuntimeLods();
    ClearSurfaceOffsets();
    MarkConfigurationDirty();
    ++m_authoredGeometryRevision;
    m_terrainVertices.clear();
    m_indices.clear();
    ReplaceIndexBuffer({});
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

} // namespace Engine::Components

