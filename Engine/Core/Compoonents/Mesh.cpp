#include "Mesh.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include <fstream>
#include <sstream>
#include <array>
#include <filesystem>
#include <stdexcept>
#include <cstring>

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

namespace
{
constexpr uint32_t kNativeMeshMagic = 0x4853454d; // "MESH"
constexpr uint32_t kNativeMeshVersion = 4;
struct LegacyVertexV2
{
    float pos[3], normal[3], uv[2], tangent[4];
};
struct LegacyVertexV3
{
    float pos[3], normal[3], uv[2], tangent[4], uv1[2], color[4];
};

std::filesystem::path ResolveMeshPath(const std::string& path)
{
    const std::filesystem::path requested(path);
    if (std::filesystem::exists(requested))
        return requested;

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
            return engineAsset;
    }

    return requested;
}

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
    m_filePath = path;  // store for serialization
    const std::filesystem::path resolvedPath = ResolveMeshPath(path);
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
                if (vi  > 0) { auto& p = positions[vi  - 1]; v.pos[0]    = p[0]; v.pos[1]    = p[1]; v.pos[2]    = p[2]; }
                if (vti > 0) { auto& uv = texcoords[vti - 1]; v.uv[0] = uv[0]; v.uv[1] = uv[1]; }
                if (vni > 0) { auto& n = normals  [vni - 1]; v.normal[0] = n[0]; v.normal[1] = n[1]; v.normal[2] = n[2]; }
                m_vertices.push_back(v);
            }
        }
    }

    UpdateBounds();
    m_ready = false;
}

void Mesh::SetDeformedVertices(const std::vector<Vertex>& vertices)
{
    if (vertices.size() != m_vertices.size())
        return;
    m_vertices = vertices;
    UpdateBounds();
    if (m_vertexBuffer)
    {
        if (void* mapped = m_vertexBuffer->Map())
        {
            std::memcpy(mapped, m_vertices.data(), m_vertices.size() * sizeof(Vertex));
            m_vertexBuffer->Unmap();
        }
    }
}

void Mesh::SetMorphData(unsigned nodeIndex, std::vector<MorphTarget> targets,
    std::vector<float> weights)
{
    m_morphNodeIndex = nodeIndex;
    m_morphTargets = std::move(targets);
    m_morphWeights = std::move(weights);
    m_morphWeights.resize(m_morphTargets.size(), 0.f);
}

void Mesh::UpdateBounds()
{
    m_hasBounds = !m_vertices.empty();
    if (!m_hasBounds)
    {
        m_boundsMin = {};
        m_boundsMax = {};
        return;
    }
    m_boundsMin = { m_vertices.front().pos[0], m_vertices.front().pos[1],
        m_vertices.front().pos[2] };
    m_boundsMax = m_boundsMin;
    for (const Vertex& vertex : m_vertices)
    {
        const glm::vec3 position(vertex.pos[0], vertex.pos[1], vertex.pos[2]);
        m_boundsMin = glm::min(m_boundsMin, position);
        m_boundsMax = glm::max(m_boundsMax, position);
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

#pragma region DX12 buffer creation and rendering

void Mesh::CreateBuffer(IGraphicsBufferFactory* bufferFactory)
{
    if (!bufferFactory || m_vertices.empty())
        return;

    const uint64_t byteSize = m_vertices.size() * sizeof(Vertex);

    // Create upload buffer through the graphics factory
    m_vertexBuffer = bufferFactory->CreateBuffer(
        IGraphicsBuffer::Usage::VertexBuffer,
        IGraphicsBuffer::AccessMode::Upload,
        byteSize,
        m_vertices.data());

    if (!m_vertexBuffer)
        throw std::runtime_error("Failed to create vertex buffer");

    m_ready = true;
}
#pragma endregion

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
