#include "Mesh.h"
#include <fstream>
#include <sstream>
#include <array>
#include <filesystem>
#include <stdexcept>

#ifndef ENGINE_ASSETS_PATH
#define ENGINE_ASSETS_PATH "Engine/Core/Assets/"
#endif

namespace
{
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
}

#pragma region OBJ file parsing helpers
static void ParseFaceToken(const std::string& t, int& vi, int& vni)
{
    vi = vni = 0;
    size_t a = t.find('/');
    if (a == std::string::npos) { vi = std::stoi(t); return; }
    vi = std::stoi(t.substr(0, a));
    size_t b = t.find('/', a + 1);
    if (b != std::string::npos && b + 1 < t.size())
        vni = std::stoi(t.substr(b + 1));
}
#pragma endregion
#pragma region Mesh implementation

void Mesh::LoadFromFile(const std::string& path)
{
    m_filePath = path;  // store for serialization
    const std::filesystem::path resolvedPath = ResolveMeshPath(path);
    std::ifstream file(resolvedPath);
    if (!file.is_open())
        throw std::runtime_error("Mesh: failed to open OBJ: " + path +
            " (resolved to " + resolvedPath.string() + ")");

    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 3>> normals;
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
        else if (token == "f")
        {
            std::string t0, t1, t2;
            ss >> t0 >> t1 >> t2;
            for (auto& tok : { t0, t1, t2 })
            {
                int vi = 0, vni = 0;
                ParseFaceToken(tok, vi, vni);
                Vertex v{};
                if (vi  > 0) { auto& p = positions[vi  - 1]; v.pos[0]    = p[0]; v.pos[1]    = p[1]; v.pos[2]    = p[2]; }
                if (vni > 0) { auto& n = normals  [vni - 1]; v.normal[0] = n[0]; v.normal[1] = n[1]; v.normal[2] = n[2]; }
                m_vertices.push_back(v);
            }
        }
    }

    m_ready = false;
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

// ---- Serialization ----------------------------------------------------------
JsonValue Mesh::Serialize() const
{
    JsonValue node = JsonValue::MakeObject();
    node.Set("type", JsonValue(std::string("Mesh")));
    node.Set("file", JsonValue(m_filePath));
    return node;
}

void Mesh::Deserialize(const JsonValue& v)
{
    m_filePath = v["file"].AsString();
    if (!m_filePath.empty())
        LoadFromFile(m_filePath);
    // CreateBuffer() must be called separately once a D3D12 device is available.
}
